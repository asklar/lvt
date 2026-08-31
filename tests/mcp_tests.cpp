// MCP server tests: drive `lvt mcp` over stdio exactly as a host would.
//
// These are separate from integration_tests.cpp because they need a very
// different harness — a live JSON-RPC conversation over pipes rather than a
// one-shot command — and because the whole file is conditional on the MCP
// server having been built.

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <wil/resource.h>

#include <windows.h>
#include "lvt_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "apps/NativeControlsFixture/native_controls_fixture_ids.h"
#include "exact_hwnd_recycle_test_support.h"

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace native_fixture = lvt::native_fixture;

namespace {

std::string get_lvt_path() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    return (fs::path(exePath).parent_path() / "lvt.exe").string();
}

USHORT pe_machine(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return IMAGE_FILE_MACHINE_UNKNOWN;

    uint16_t dosMagic = 0;
    file.read(reinterpret_cast<char*>(&dosMagic), sizeof(dosMagic));
    if (!file || dosMagic != IMAGE_DOS_SIGNATURE)
        return IMAGE_FILE_MACHINE_UNKNOWN;

    file.seekg(0x3c);
    uint32_t peOffset = 0;
    file.read(reinterpret_cast<char*>(&peOffset), sizeof(peOffset));
    file.seekg(peOffset);
    uint32_t peSignature = 0;
    USHORT machine = IMAGE_FILE_MACHINE_UNKNOWN;
    file.read(reinterpret_cast<char*>(&peSignature), sizeof(peSignature));
    file.read(reinterpret_cast<char*>(&machine), sizeof(machine));
    return file && peSignature == IMAGE_NT_SIGNATURE
        ? machine
        : IMAGE_FILE_MACHINE_UNKNOWN;
}

constexpr USHORT test_machine() {
#if defined(_M_ARM64)
    return IMAGE_FILE_MACHINE_ARM64;
#elif defined(_M_IX86)
    return IMAGE_FILE_MACHINE_I386;
#elif defined(_M_X64)
    return IMAGE_FILE_MACHINE_AMD64;
#else
    return IMAGE_FILE_MACHINE_UNKNOWN;
#endif
}

const char* machine_name(USHORT machine) {
    switch (machine) {
    case IMAGE_FILE_MACHINE_ARM64:
        return "arm64";
    case IMAGE_FILE_MACHINE_I386:
        return "x86";
    case IMAGE_FILE_MACHINE_AMD64:
        return "x64";
    default:
        return "unknown";
    }
}

std::string run_command(const std::string& command) {
    std::string output;
    char buffer[4096]{};
    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe)
        return {};
    while (fgets(buffer, static_cast<int>(sizeof(buffer)), pipe))
        output += buffer;
    _pclose(pipe);
    return output;
}

std::string cmd_escape_arg(const std::string& argument) {
    std::string escaped;
    escaped.reserve(argument.size() * 2);
    for (const char value : argument) {
        if (value == '|' || value == '&' || value == '<' ||
            value == '>' || value == '^') {
            escaped += '^';
        }
        escaped += value;
    }
    return escaped;
}

std::string trim_crlf(const std::string& value) {
    const auto end = value.find_last_not_of("\r\n");
    return end == std::string::npos
        ? std::string()
        : value.substr(0, end + 1);
}

std::string read_text_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
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

class NativePublicationGate {
public:
    explicit NativePublicationGate(const char* method) {
        const auto serial = nextSerial_.fetch_add(1);
        base_ =
            "Local\\lvt-native-publication-" +
            std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(serial);
        entered_.reset(CreateEventA(
            nullptr, TRUE, FALSE, (base_ + "-entered").c_str()));
        release_.reset(CreateEventA(
            nullptr, TRUE, FALSE, (base_ + "-release").c_str()));
        if (entered_ && release_) {
            gateVariable_ = std::make_unique<ScopedEnvironmentVariable>(
                "LVT_TEST_NATIVE_PUBLICATION_GATE", base_);
            methodVariable_ = std::make_unique<ScopedEnvironmentVariable>(
                "LVT_TEST_NATIVE_PUBLICATION_METHOD", method);
        }
    }

    ~NativePublicationGate() {
        release();
    }

    bool valid() const {
        return entered_ && release_ &&
               gateVariable_ && methodVariable_;
    }

    bool wait_until_entered() const {
        return entered_ &&
               WaitForSingleObject(entered_.get(), 10000) ==
                   WAIT_OBJECT_0;
    }

    void release() {
        if (release_)
            SetEvent(release_.get());
    }

private:
    inline static std::atomic<uint64_t> nextSerial_{1};
    std::string base_;
    wil::unique_handle entered_;
    wil::unique_handle release_;
    std::unique_ptr<ScopedEnvironmentVariable> gateVariable_;
    std::unique_ptr<ScopedEnvironmentVariable> methodVariable_;
};

fs::path plugin_stats_path(const std::string& testName) {
    return fs::path(get_lvt_path()).parent_path() /
           ("fake-plugin-mcp-" + testName + "-" +
            std::to_string(GetCurrentProcessId()) + ".log");
}

std::vector<std::string> read_plugin_stats(const fs::path& path) {
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

size_t count_plugin_stats(
    const std::vector<std::string>& lines, const std::string& prefix) {
    return static_cast<size_t>(std::count_if(
        lines.begin(), lines.end(), [&prefix](const std::string& line) {
            return line.rfind(prefix, 0) == 0;
        }));
}

int count_tap_log_lines(const char* text, DWORD pid) {
    wchar_t tempPath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempPath) == 0)
        return 0;
    std::ifstream log(std::wstring(tempPath) + L"lvt_tap.log", std::ios::binary);
    int count = 0;
    std::string line;
    const auto processMarker =
        "][" + std::to_string(pid) + "][";
    while (std::getline(log, line)) {
        if (line.find(processMarker) != std::string::npos &&
            line.find(text) != std::string::npos)
            ++count;
    }
    return count;
}

int count_tap_set_site_calls(DWORD pid) {
    return count_tap_log_lines("SetSite called", pid);
}

int count_tap_get_enums_calls(DWORD pid) {
    return count_tap_log_lines("GetEnums called once", pid);
}

// A live MCP conversation with `lvt mcp` over anonymous pipes.
//
// Reading is done on a worker thread: a blocking ReadFile on a pipe cannot be
// cancelled, so without one a server that never answers would wedge the whole
// test run rather than failing one test.
class McpClient {
public:
    explicit McpClient(bool allowInput) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};

        HANDLE inRead = nullptr, inWrite = nullptr;
        HANDLE outRead = nullptr, outWrite = nullptr;
        if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0))
            return;
        // Only the ends the child needs may be inheritable, or the child holds
        // a copy of our end and the pipe never reports EOF when it exits.
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

        childStdin_.reset(inRead);
        stdin_.reset(inWrite);
        stdout_.reset(outRead);
        childStdout_.reset(outWrite);

        STARTUPINFOA si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = childStdin_.get();
        si.hStdOutput = childStdout_.get();
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION pi{};
        std::string cmd = "\"" + get_lvt_path() + "\" mcp";
        if (allowInput)
            cmd += " --allow-input";
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi))
            return;
        process_.reset(pi.hProcess);
        thread_.reset(pi.hThread);
        // Our copies of the child's ends must go, or stdout never reaches EOF.
        childStdin_.reset();
        childStdout_.reset();

        reader_ = std::thread([this] { read_loop(); });
        started_ = true;
    }

    ~McpClient() { shutdown(); }

    bool started() const { return started_; }

    void shutdown() {
        if (!started_)
            return;
        stdin_.reset();  // EOF tells the server to stop
        // Generous, because this is the harness's patience rather than the
        // property under test: a call already in flight can keep the server
        // alive for a few seconds, and the timing assertions live in the tests
        // that care. Terminating early would mask a clean exit as a failure.
        if (process_ && WaitForSingleObject(process_.get(), 20000) == WAIT_TIMEOUT)
            TerminateProcess(process_.get(), 1);
        stopping_ = true;
        stdout_.reset();
        if (reader_.joinable())
            reader_.join();
        started_ = false;
    }

    DWORD exit_code() const {
        DWORD code = STILL_ACTIVE;
        if (process_)
            GetExitCodeProcess(process_.get(), &code);
        return code;
    }

    void terminate_server() {
        if (!started_)
            return;
        if (process_) {
            TerminateProcess(process_.get(), 1);
            WaitForSingleObject(process_.get(), 5000);
        }
        shutdown();
    }

    void notify(const std::string& method, const json& params = json()) {
        json message{{"jsonrpc", "2.0"}, {"method", method}};
        if (!params.is_null())
            message["params"] = params;
        write(message);
    }

    // Sends a request and returns its response, or a null json on timeout.
    json request(const std::string& method, const json& params = json::object()) {
        return await(send_request(method, params));
    }

    // Split form, for firing several requests before waiting on any of them.
    // rmcp dispatches each request with tokio::spawn, so requests sent this way
    // genuinely overlap on the runtime's worker threads.
    int send_request(const std::string& method, const json& params = json::object()) {
        const int id = ++nextId_;
        auto requestParams = params;
        if (protocolVersion_ >= "2026-07-28") {
            requestParams["_meta"] = {
                {"io.modelcontextprotocol/protocolVersion", protocolVersion_},
                {"io.modelcontextprotocol/clientCapabilities", json::object()},
            };
        }
        write(json{{"jsonrpc", "2.0"},
                   {"id", id},
                   {"method", method},
                   {"params", std::move(requestParams)}});
        return id;
    }

    json await_response(int id) { return await(id); }

    // Wait for a server-to-client notification already arriving on the stdio
    // transport. This sends no request: a passing test proves the server
    // initiated the message rather than answering a client poll.
    json await_notification(const std::string& method, int timeoutSeconds = 30) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (size_t i = 0; i < messages_.size(); ++i) {
                    if (!messages_[i].contains("id") &&
                        messages_[i].value("method", "") == method) {
                        auto found = messages_[i];
                        messages_.erase(messages_.begin() + static_cast<ptrdiff_t>(i));
                        return found;
                    }
                }
            }
            if (eof_ || std::chrono::steady_clock::now() >= deadline)
                return json();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Convenience for tools/call that returns the tool's parsed JSON payload.
    // `isError` reports whether the tool reported failure, which is distinct
    // from a protocol error and is how lvt surfaces "no such element".
    json call_tool(const std::string& name, const json& args, bool* isError = nullptr) {
        auto response = request("tools/call", json{{"name", name}, {"arguments", args}});
        if (response.is_null() || !response.contains("result")) {
            if (isError)
                *isError = true;
            return response;
        }
        const auto& result = response["result"];
        if (isError)
            *isError = result.value("isError", false);
        if (!result.contains("content") || result["content"].empty())
            return json::object();
        for (const auto& block : result["content"]) {
            if (block.value("type", "") == "text")
                return json::parse(block.value("text", "{}"), nullptr, false);
        }
        return json::object();
    }

    // The raw content blocks, for tools whose result is not just text.
    json call_tool_content(const std::string& name, const json& args) {
        auto response = request("tools/call", json{{"name", name}, {"arguments", args}});
        if (response.is_null() || !response.contains("result"))
            return json::array();
        return response["result"].value("content", json::array());
    }

    // The whole `result` object, including `structuredContent` and `isError`.
    // The other helpers deliberately hide the envelope, which is exactly what a
    // test about the envelope needs to see.
    json call_tool_result(const std::string& name, const json& args) {
        auto response = request("tools/call", json{{"name", name}, {"arguments", args}});
        if (response.is_null() || !response.contains("result"))
            return json::object();
        return response["result"];
    }

    // initialize + initialized, the handshake every session begins with.
    bool handshake(
        json* serverInfo = nullptr,
        const std::string& protocolVersion = "2025-06-18") {
        auto response = request("initialize",
                                json{{"protocolVersion", protocolVersion},
                                     {"capabilities", json::object()},
                                     {"clientInfo", json{{"name", "lvt-tests"}, {"version", "1"}}}});
        if (response.is_null() || !response.contains("result"))
            return false;
        if (serverInfo)
            *serverInfo = response["result"];
        protocolVersion_ = response["result"].value("protocolVersion", protocolVersion);
        notify("notifications/initialized");
        return true;
    }

private:
    void write(const json& message) {
        const auto text = message.dump() + "\n";
        // Requests can be issued from several threads at once by the
        // concurrency tests, and a torn write would corrupt the stream itself
        // rather than testing the server.
        std::lock_guard<std::mutex> lock(writeMutex_);
        DWORD written = 0;
        WriteFile(stdin_.get(), text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
    }

    void read_loop() {
        std::string buffer;
        char chunk[4096];
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(stdout_.get(), chunk, sizeof(chunk), &read, nullptr) || read == 0)
                break;
            buffer.append(chunk, read);
            size_t newline;
            while ((newline = buffer.find('\n')) != std::string::npos) {
                auto line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                if (line.empty())
                    continue;
                auto parsed = json::parse(line, nullptr, false);
                if (parsed.is_discarded())
                    continue;
                std::lock_guard<std::mutex> lock(mutex_);
                messages_.push_back(std::move(parsed));
            }
        }
        eof_ = true;
    }

    json await(int id) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (size_t i = 0; i < messages_.size(); ++i) {
                    if (messages_[i].value("id", -1) == id) {
                        auto found = messages_[i];
                        messages_.erase(messages_.begin() + static_cast<ptrdiff_t>(i));
                        return found;
                    }
                }
            }
            if (eof_ || std::chrono::steady_clock::now() >= deadline)
                return json();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    wil::unique_handle stdin_, stdout_, childStdin_, childStdout_;
    wil::unique_process_handle process_;
    wil::unique_handle thread_;
    std::thread reader_;
    std::mutex mutex_;
    std::mutex writeMutex_;
    std::vector<json> messages_;
    std::atomic<bool> eof_{false};
    std::atomic<bool> stopping_{false};
    bool started_ = false;
    std::atomic<int> nextId_{0};
    std::string protocolVersion_;
};

void collect_json_elements(const json& element, std::vector<const json*>& out) {
    if (!element.is_object())
        return;
    out.push_back(&element);
    const auto children = element.find("children");
    if (children == element.end() || !children->is_array())
        return;
    for (const auto& child : *children)
        collect_json_elements(child, out);
}

const json* find_property_descriptor(const json& snapshot, const std::string& name) {
    const auto descriptors = snapshot.find("descriptors");
    if (descriptors == snapshot.end() || !descriptors->is_array())
        return nullptr;
    for (const auto& descriptor : *descriptors) {
        if (descriptor.value("name", "") == name)
            return &descriptor;
    }
    return nullptr;
}

const json* find_property_value(const json& snapshot, const std::string& descriptorId) {
    const auto values = snapshot.find("values");
    if (values == snapshot.end() || !values->is_array())
        return nullptr;
    for (const auto& value : *values) {
        if (value.value("descriptorId", "") == descriptorId)
            return &value;
    }
    return nullptr;
}

std::string typed_property_value(const json& snapshot, const std::string& name) {
    const auto* descriptor = find_property_descriptor(snapshot, name);
    if (!descriptor)
        return {};
    const auto* value =
        find_property_value(snapshot, descriptor->value("descriptorId", ""));
    return value ? value->value("value", "") : std::string();
}

void expect_typed_property_session_disconnected(const json& result) {
    EXPECT_FALSE(result.value("ok", true)) << result.dump(2);
    EXPECT_EQ(
        result.value("errorCode", ""),
        "typed_property_session_disconnected")
        << result.dump(2);
    EXPECT_EQ(result.value("errorDisposition", ""), "ownershipLost")
        << result.dump(2);
    EXPECT_FALSE(result.value("retryable", true)) << result.dump(2);
}

bool descriptor_has_choice(const json& descriptor, const std::string& value) {
    for (const auto& choice : descriptor.value("choices", json::array())) {
        if (choice.value("value", "") == value)
            return true;
    }
    return false;
}

// A JSON Schema checker covering exactly the keywords lvt's output schemas use:
// type, const, enum, required, properties, items, additionalProperties, anyOf.
//
// Pulling in a full validator would be a dependency for one test. The point here
// is not to be a conformant validator but to catch the mistake that matters:
// declaring an `outputSchema` a real response does not satisfy. A client that
// does validate will apply these same keywords, so checking them is checking the
// promise we made.
bool schema_allows(const json& schema, const json& value, std::string& why,
                   const std::string& path = "") {
    const auto fail = [&](const std::string& message) {
        why = (path.empty() ? std::string("<root>") : path) + ": " + message;
        return false;
    };

    if (schema.contains("anyOf")) {
        std::string first;
        for (const auto& branch : schema["anyOf"]) {
            std::string ignored;
            if (schema_allows(branch, value, ignored, path))
                return true;
            if (first.empty())
                first = ignored;
        }
        return fail("matched none of the anyOf branches (" + first + ")");
    }

    if (schema.contains("const") && value != schema["const"])
        return fail("expected the constant " + schema["const"].dump());

    if (schema.contains("enum")) {
        bool found = false;
        for (const auto& allowed : schema["enum"])
            found = found || allowed == value;
        if (!found)
            return fail("value " + value.dump() + " is not one of " + schema["enum"].dump());
    }

    if (schema.contains("type")) {
        const auto expected = schema["type"].get<std::string>();
        const bool ok = (expected == "object" && value.is_object()) ||
                        (expected == "array" && value.is_array()) ||
                        (expected == "string" && value.is_string()) ||
                        (expected == "boolean" && value.is_boolean()) ||
                        (expected == "integer" && value.is_number_integer()) ||
                        (expected == "number" && value.is_number());
        if (!ok)
            return fail("expected type " + expected + " but got " + std::string(value.type_name()));
    }

    if (value.is_object()) {
        for (const auto& field : schema.value("required", json::array())) {
            if (!value.contains(field.get<std::string>()))
                return fail("missing required field '" + field.get<std::string>() + "'");
        }
        const auto properties = schema.value("properties", json::object());
        for (const auto& [name, member] : value.items()) {
            const auto declared = properties.find(name);
            if (declared != properties.end()) {
                if (!schema_allows(*declared, member, why, path + "/" + name))
                    return false;
            } else if (schema.contains("additionalProperties") &&
                       schema["additionalProperties"].is_object()) {
                if (!schema_allows(schema["additionalProperties"], member, why, path + "/" + name))
                    return false;
            }
        }
    }

    if (value.is_array() && schema.contains("items")) {
        for (size_t i = 0; i < value.size(); ++i) {
            if (!schema_allows(schema["items"], value[i], why,
                               path + "[" + std::to_string(i) + "]"))
                return false;
        }
    }
    return true;
}

std::map<std::string, json> output_schemas(McpClient& client) {
    // The response is bound to a named value first. Iterating directly over
    // `request(...)["result"]["tools"]` binds a reference into a temporary that
    // dies at the end of the statement, and the loop then reads freed memory —
    // which showed up as a map full of garbage keys rather than as a crash.
    const auto response = client.request("tools/list");
    std::map<std::string, json> schemas;
    for (const auto& tool : response["result"]["tools"]) {
        if (tool.contains("outputSchema"))
            schemas[tool.value("name", "")] = tool["outputSchema"];
    }
    return schemas;
}

std::vector<std::string> tool_names(const json& toolsResult) {    std::vector<std::string> names;
    for (const auto& tool : toolsResult.value("tools", json::array()))
        names.push_back(tool.value("name", ""));
    std::sort(names.begin(), names.end());
    return names;
}

bool has_tool(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

class ManagedSampleProcess {
public:
    bool start(const fs::path& executable) {
        if (!fs::exists(executable))
            return false;
        STARTUPINFOA startup{sizeof(startup)};
        PROCESS_INFORMATION processInfo{};
        std::string command = "\"" + executable.string() + "\"";
        if (!CreateProcessA(
                nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                executable.parent_path().string().c_str(), &startup, &processInfo)) {
            return false;
        }
        process_.reset(processInfo.hProcess);
        thread_.reset(processInfo.hThread);
        pid_ = processInfo.dwProcessId;
        WaitForInputIdle(process_.get(), 5000);

        for (int attempt = 0; attempt < 30 && !hwnd_; ++attempt) {
            EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
                auto* self = reinterpret_cast<ManagedSampleProcess*>(parameter);
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

    ~ManagedSampleProcess() {
        stop();
    }

    void stop() {
        if (process_) {
            TerminateProcess(process_.get(), 0);
            WaitForSingleObject(process_.get(), 5000);
        }
        process_.reset();
        thread_.reset();
        pid_ = 0;
        hwnd_ = nullptr;
    }

    DWORD pid() const { return pid_; }
    HWND hwnd() const { return hwnd_; }

    std::string hwnd_string() const {
        char buffer[32];
        snprintf(
            buffer, sizeof(buffer), "0x%llX",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd_)));
        return buffer;
    }

private:
    wil::unique_process_handle process_;
    wil::unique_handle thread_;
    DWORD pid_ = 0;
    HWND hwnd_ = nullptr;
};

const json* find_fake_plugin_node(const json& root) {
    std::vector<const json*> elements;
    collect_json_elements(root, elements);
    for (const auto* element : elements) {
        if (element->value("type", "") == "FakePluginNode")
            return element;
    }
    return nullptr;
}

std::unique_ptr<McpClient> start_plugin_mcp(
    const fs::path& pluginDirectory, const fs::path& statsPath,
    const std::string& failOpenAt, const std::string& failGetAt,
    const std::string& delayMs, const std::string& emitEvents,
    const std::string& delayDetectAt = "0",
    const std::string& detectDelayMs = "0") {
    ScopedEnvironmentVariable pluginPath(
        "LVT_PLUGIN_DIR", pluginDirectory.string());
    ScopedEnvironmentVariable enabled("LVT_FAKE_PLUGIN_ENABLE", "1");
    ScopedEnvironmentVariable state(
        "LVT_FAKE_PLUGIN_STATE", statsPath.string());
    ScopedEnvironmentVariable fail(
        "LVT_FAKE_PLUGIN_FAIL_GET_AT", failGetAt);
    ScopedEnvironmentVariable malformed(
        "LVT_FAKE_PLUGIN_MALFORMED_GET_AT", "0");
    ScopedEnvironmentVariable failOpen(
        "LVT_FAKE_PLUGIN_FAIL_OPEN_AT", failOpenAt);
    ScopedEnvironmentVariable delay(
        "LVT_FAKE_PLUGIN_GET_DELAY_MS", delayMs);
    ScopedEnvironmentVariable events(
        "LVT_FAKE_PLUGIN_EMIT_EVENTS", emitEvents);
    ScopedEnvironmentVariable detectFile(
        "LVT_FAKE_PLUGIN_DETECT_FILE", "");
    ScopedEnvironmentVariable detectAt(
        "LVT_FAKE_PLUGIN_DELAY_DETECT_AT", delayDetectAt);
    ScopedEnvironmentVariable detectDelay(
        "LVT_FAKE_PLUGIN_DETECT_DELAY_MS", detectDelayMs);
    return std::make_unique<McpClient>(false);
}

TEST(McpPluginPersistent, ReusesPollsReconnectsAndDisconnectsConcurrently) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto statsPath = plugin_stats_path("persistent");
    std::error_code ec;
    fs::remove(statsPath, ec);

    auto client = start_plugin_mcp(
        LVT_FAKE_PLUGIN_V2_DIR, statsPath, "1", "3", "250", "1");
    ASSERT_TRUE(client->started());
    ASSERT_TRUE(client->handshake());

    auto connected = client->call_tool(
        "connect",
        json{{"hwnd", sample.hwnd_string()},
             {"title", "mcp-filter"},
             {"mode", "visual"}});
    const std::string session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);

    auto first =
        client->call_tool("get_visual_tree", json{{"session", session}});
    auto second =
        client->call_tool("get_visual_tree", json{{"session", session}});
    auto reconnected =
        client->call_tool("get_visual_tree", json{{"session", session}});
    ASSERT_TRUE(first.contains("root")) << first.dump(2);
    ASSERT_TRUE(second.contains("root")) << second.dump(2);
    ASSERT_TRUE(reconnected.contains("root")) << reconnected.dump(2);
    for (const auto* tree : {&first, &second, &reconnected}) {
        const auto* node = find_fake_plugin_node((*tree)["root"]);
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(
            node->value("properties", json::object()).value("source", ""),
            "persistent");
        EXPECT_EQ(
            node->value("properties", json::object()).value("filter", ""),
            "mcp-filter");
    }

    const int readId = client->send_request(
        "tools/call",
        json{{"name", "get_visual_tree"},
             {"arguments", json{{"session", session}}}});
    const auto readStartedDeadline = GetTickCount64() + 5000;
    while (GetTickCount64() < readStartedDeadline) {
        if (count_plugin_stats(read_plugin_stats(statsPath), "get ") >= 5)
            break;
        Sleep(10);
    }
    ASSERT_GE(
        count_plugin_stats(read_plugin_stats(statsPath), "get "), 5u);
    const int disconnectId = client->send_request(
        "tools/call",
        json{{"name", "disconnect"},
             {"arguments", json{{"session", session}}}});

    const auto readResponse = client->await_response(readId);
    const auto disconnectResponse = client->await_response(disconnectId);
    ASSERT_TRUE(readResponse.contains("result")) << readResponse.dump(2);
    ASSERT_TRUE(disconnectResponse.contains("result"))
        << disconnectResponse.dump(2);
    EXPECT_FALSE(readResponse["result"].value("isError", false));
    EXPECT_FALSE(disconnectResponse["result"].value("isError", false));

    const auto stats = read_plugin_stats(statsPath);
    EXPECT_EQ(count_plugin_stats(stats, "open "), 3u);
    EXPECT_EQ(count_plugin_stats(stats, "open_failed "), 1u);
    EXPECT_EQ(count_plugin_stats(stats, "close"), 2u);
    EXPECT_EQ(count_plugin_stats(stats, "enrich "), 0u);
    const auto gets = count_plugin_stats(stats, "get ");
    const auto failures = count_plugin_stats(stats, "get_failed ");
    EXPECT_EQ(gets, 5u);
    EXPECT_EQ(failures, 1u);
    EXPECT_EQ(count_plugin_stats(stats, "free"), gets - failures);
    EXPECT_EQ(count_plugin_stats(stats, "poll"), 4u)
        << "five tree attempts with one failed build must poll only the four "
           "successful plugin snapshots";
    EXPECT_EQ(count_plugin_stats(stats, "events_free"), 4u);
    for (const auto& line : stats) {
        if (line.rfind("get ", 0) == 0)
            EXPECT_NE(line.find("filter=mcp-filter"), std::string::npos);
    }

    client->shutdown();
    fs::remove(statsPath, ec);
}

TEST(McpPluginPersistent, CorrelationScopesVisualAndUiaEventPolling) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto pluginStatsPath =
        plugin_stats_path("correlation-scope-plugin");
    const auto uiaStatsPath =
        plugin_stats_path("correlation-scope-uia");
    std::error_code ec;
    fs::remove(pluginStatsPath, ec);
    fs::remove(uiaStatsPath, ec);
    ScopedEnvironmentVariable uiaStats(
        "LVT_TEST_UIA_EVENT_STATS", uiaStatsPath.string());

    auto client = start_plugin_mcp(
        LVT_FAKE_PLUGIN_V2_DIR, pluginStatsPath,
        "0", "0", "0", "1");
    ASSERT_TRUE(client->started());
    ASSERT_TRUE(client->handshake());

    struct FixtureWindow {
        DWORD pid;
        HWND root = nullptr;
        HWND edit = nullptr;
    } fixture{sample.pid()};
    EnumWindows(
        [](HWND candidate, LPARAM parameter) -> BOOL {
            auto* fixture =
                reinterpret_cast<FixtureWindow*>(parameter);
            DWORD owner = 0;
            GetWindowThreadProcessId(candidate, &owner);
            if (owner != fixture->pid)
                return TRUE;
            const HWND edit = GetDlgItem(
                candidate, native_fixture::kEditId);
            if (!edit)
                return TRUE;
            fixture->root = candidate;
            fixture->edit = edit;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&fixture));
    ASSERT_TRUE(IsWindow(fixture.root));
    ASSERT_TRUE(IsWindow(fixture.edit));
    char fixtureHwnd[32]{};
    snprintf(
        fixtureHwnd, sizeof(fixtureHwnd), "0x%llX",
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(fixture.root)));

    const auto visualConnect = client->call_tool(
        "connect",
        json{{"hwnd", fixtureHwnd},
             {"mode", "visual"}});
    const auto uiaConnect = client->call_tool(
        "connect",
        json{{"hwnd", fixtureHwnd},
             {"mode", "uia"}});
    const auto visualSession =
        visualConnect.value("session", "");
    const auto uiaSession = uiaConnect.value("session", "");
    ASSERT_FALSE(visualSession.empty()) << visualConnect.dump(2);
    ASSERT_FALSE(uiaSession.empty()) << uiaConnect.dump(2);

    auto initialUia = client->call_tool(
        "get_uia_tree", json{{"session", uiaSession}});
    ASSERT_TRUE(initialUia.contains("root")) << initialUia.dump(2);

    auto pluginBefore = read_plugin_stats(pluginStatsPath);
    auto uiaBefore = read_plugin_stats(uiaStatsPath);
    auto correlated = client->call_tool(
        "get_visual_tree",
        json{{"session", visualSession}, {"correlate", true}});
    ASSERT_TRUE(correlated.contains("root")) << correlated.dump(2);
    auto pluginAfterCorrelation =
        read_plugin_stats(pluginStatsPath);
    auto uiaAfterCorrelation =
        read_plugin_stats(uiaStatsPath);
    EXPECT_EQ(
        count_plugin_stats(pluginAfterCorrelation, "poll") -
            count_plugin_stats(pluginBefore, "poll"),
        1u)
        << "one correlated visual request must poll visual plugins once, "
           "not again during its UIA correlation walk";
    EXPECT_EQ(
        count_plugin_stats(uiaAfterCorrelation, "poll") -
            count_plugin_stats(uiaBefore, "poll"),
        1u);
    EXPECT_EQ(
        count_plugin_stats(uiaAfterCorrelation, "refresh") -
            count_plugin_stats(uiaBefore, "refresh"),
        1u);

    HWND edit = fixture.edit;
    ASSERT_TRUE(IsWindow(edit));
    wchar_t original[256]{};
    GetWindowTextW(
        edit, original, static_cast<int>(_countof(original)));
    ASSERT_TRUE(SetWindowTextW(
        edit, L"queued UIA event must survive visual polling"));
    NotifyWinEvent(
        EVENT_OBJECT_VALUECHANGE, edit,
        OBJID_CLIENT, CHILDID_SELF);
    Sleep(500);

    pluginBefore = pluginAfterCorrelation;
    uiaBefore = uiaAfterCorrelation;
    auto plainVisual = client->call_tool(
        "get_visual_tree",
        json{{"session", visualSession}});
    ASSERT_TRUE(plainVisual.contains("root"))
        << plainVisual.dump(2);
    const auto pluginAfterVisual =
        read_plugin_stats(pluginStatsPath);
    const auto uiaAfterVisual =
        read_plugin_stats(uiaStatsPath);
    EXPECT_EQ(
        count_plugin_stats(pluginAfterVisual, "poll") -
            count_plugin_stats(pluginBefore, "poll"),
        1u);
    EXPECT_EQ(
        count_plugin_stats(uiaAfterVisual, "poll"),
        count_plugin_stats(uiaBefore, "poll"))
        << "a visual-only snapshot consumed a retained UIA hint";
    EXPECT_EQ(
        count_plugin_stats(uiaAfterVisual, "refresh"),
        count_plugin_stats(uiaBefore, "refresh"));

    json refreshedUia;
    for (int attempt = 0;
         attempt < 3 && !refreshedUia.contains("root");
         ++attempt) {
        if (attempt > 0)
            Sleep(static_cast<DWORD>(150 * attempt));
        refreshedUia = client->call_tool(
            "get_uia_tree", json{{"session", uiaSession}});
    }
    ASSERT_TRUE(refreshedUia.contains("root"))
        << refreshedUia.dump(2);
    const auto pluginAfterUia =
        read_plugin_stats(pluginStatsPath);
    const auto uiaAfterUia =
        read_plugin_stats(uiaStatsPath);
    EXPECT_EQ(
        count_plugin_stats(pluginAfterUia, "poll"),
        count_plugin_stats(pluginAfterVisual, "poll"))
        << "a UIA snapshot polled a visual plugin";
    EXPECT_EQ(
        count_plugin_stats(uiaAfterUia, "poll") -
            count_plugin_stats(uiaAfterVisual, "poll"),
        1u);
    EXPECT_EQ(
        count_plugin_stats(uiaAfterUia, "refresh") -
            count_plugin_stats(uiaAfterVisual, "refresh"),
        1u);
    bool retainedSnapshot = false;
    for (size_t index = uiaAfterVisual.size();
         index < uiaAfterUia.size(); ++index) {
        retainedSnapshot =
            retainedSnapshot ||
            (uiaAfterUia[index].rfind("poll ", 0) == 0 &&
             uiaAfterUia[index].find("snapshot=1") !=
                 std::string::npos);
    }
    EXPECT_TRUE(retainedSnapshot)
        << "the UIA hint queued after correlation was not retained "
           "until the next UIA snapshot";

    SetWindowTextW(edit, original);
    client->shutdown();
    fs::remove(pluginStatsPath, ec);
    fs::remove(uiaStatsPath, ec);
}

TEST(McpUiaIdentity, TransientPersistentFailureUsesIdentityBoundFallback) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    ScopedEnvironmentVariable failConnected(
        "LVT_TEST_UIA_FAIL_CONNECTED_TREE_ONCE", "1");
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    const auto connected = client.call_tool(
        "connect",
        json{{"hwnd", sample.hwnd_string()},
             {"mode", "uia"}});
    const auto session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);

    const auto tree = client.call_tool(
        "get_uia_tree", json{{"session", session}});
    EXPECT_TRUE(tree.contains("root"))
        << "an unchanged target must preserve one-shot fallback: "
        << tree.dump(2);
}

TEST(McpUiaIdentity, ReplacementDuringFallbackFailsOwnershipLost) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));

    const std::string base =
        "Local\\LvtMcpUiaFallback_" +
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
    ScopedEnvironmentVariable failConnected(
        "LVT_TEST_UIA_FAIL_CONNECTED_TREE_ONCE", "1");
    ScopedEnvironmentVariable gate(
        "LVT_TEST_UIA_ONE_SHOT_AFTER_ELEMENT_GATE", base);

    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto connected = client.call_tool(
        "connect",
        json{{"hwnd", sample.hwnd_string()},
             {"mode", "uia"}});
    const auto session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);

    const int request = client.send_request(
        "tools/call",
        json{{"name", "get_uia_tree"},
             {"arguments", json{{"session", session}}}});
    ASSERT_EQ(
        WaitForSingleObject(entered.get(), 10000),
        WAIT_OBJECT_0)
        << "the MCP fallback did not reach ElementFromHandle";

    sample.stop();
    ManagedSampleProcess replacement;
    ASSERT_TRUE(replacement.start(
        NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    SetEvent(release.get());

    const auto response = client.await_response(request);
    ASSERT_TRUE(response.contains("result"))
        << response.dump(2);
    EXPECT_TRUE(response["result"].value("isError", false))
        << response.dump(2);
    EXPECT_NE(
        response.dump().find("ownershipLost"),
        std::string::npos)
        << response.dump(2);
}

TEST(
    McpUiaIdentity,
    ExactRecycleHardFailureDrainsPendingRequestAndServerRecovers) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    struct FixtureWindow {
        DWORD pid;
        HWND root = nullptr;
    } fixture{sample.pid()};
    EnumWindows(
        [](HWND candidate, LPARAM parameter) -> BOOL {
            auto* fixture =
                reinterpret_cast<FixtureWindow*>(parameter);
            DWORD owner = 0;
            GetWindowThreadProcessId(candidate, &owner);
            if (owner == fixture->pid &&
                GetDlgItem(candidate, native_fixture::kEditId)) {
                fixture->root = candidate;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&fixture));
    ASSERT_TRUE(IsWindow(fixture.root));
    const HWND edit =
        GetDlgItem(fixture.root, native_fixture::kEditId);
    ASSERT_TRUE(IsWindow(edit));
    char editHwnd[32]{};
    snprintf(
        editHwnd, sizeof(editHwnd), "0x%llX",
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(edit)));

    const std::string base =
        "Local\\LvtMcpHardRecycle_" +
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
    ScopedEnvironmentVariable failConnected(
        "LVT_TEST_UIA_FAIL_CONNECTED_TREE_ONCE", "1");
    ScopedEnvironmentVariable gate(
        "LVT_TEST_UIA_ONE_SHOT_AFTER_ELEMENT_GATE", base);

    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto connected = client.call_tool(
        "connect",
        json{{"hwnd", editHwnd}, {"mode", "uia"}});
    const auto session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);

    const int request = client.send_request(
        "tools/call",
        json{{"name", "get_uia_tree"},
             {"arguments", json{{"session", session}}}});
    lvt::test_support::ScopedEventSignal releaseOnExit(
        release.get());
    ASSERT_EQ(
        WaitForSingleObject(entered.get(), 10000),
        WAIT_OBJECT_0)
        << "the MCP fallback did not reach ElementFromHandle";

    const bool suppressedBefore =
        lvt::test_support::exact_hwnd_recycle_search_suppressed();
    lvt::test_support::ExactHwndRecycleOptions options;
    options.testMode =
        lvt::test_support::ExactHwndRecycleTestMode::
            forceHardFailure;
    const auto recycled =
        lvt::test_support::recycle_event_child_exact(
            fixture.root, edit, native_fixture::kEditId,
            options);
    releaseOnExit.signal();
    const auto response = client.await_response(request);

    ASSERT_TRUE(response.contains("result"))
        << response.dump(2);
    EXPECT_TRUE(response["result"].value("isError", false))
        << response.dump(2);
    EXPECT_NE(
        response.dump().find("ownershipLost"),
        std::string::npos)
        << response.dump(2);
    ASSERT_EQ(
        recycled.outcome,
        lvt::test_support::ExactHwndRecycleOutcome::hardFailure)
        << recycled.reason;
    EXPECT_EQ(
        recycled.failureStage,
        native_fixture::ExactHwndRecycleFailureStage::
            createSearchCandidate);
    EXPECT_EQ(recycled.peakHeldWindows, 0u);
    EXPECT_EQ(recycled.remainingHeldWindows, 0u);
    ASSERT_TRUE(IsWindow(recycled.replacement));
    EXPECT_NE(recycled.replacement, edit);
    EXPECT_EQ(
        lvt::test_support::exact_hwnd_recycle_search_suppressed(),
        suppressedBefore);

    char replacementHwnd[32]{};
    snprintf(
        replacementHwnd, sizeof(replacementHwnd), "0x%llX",
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(
                recycled.replacement)));
    const auto replacementConnected = client.call_tool(
        "connect",
        json{{"hwnd", replacementHwnd}, {"mode", "uia"}});
    const auto replacementSession =
        replacementConnected.value("session", "");
    ASSERT_FALSE(replacementSession.empty())
        << replacementConnected.dump(2);
    const auto replacementTree = client.call_tool(
        "get_uia_tree",
        json{{"session", replacementSession}});
    EXPECT_TRUE(replacementTree.contains("root"))
        << replacementTree.dump(2);
}

TEST(McpUiaIdentity, ElementlessKeyboardActionsReturnStructuredOwnershipLost) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    struct FixtureWindow {
        DWORD pid;
        HWND root = nullptr;
    } fixture{sample.pid()};
    EnumWindows(
        [](HWND candidate, LPARAM parameter) -> BOOL {
            auto* fixture =
                reinterpret_cast<FixtureWindow*>(parameter);
            DWORD owner = 0;
            GetWindowThreadProcessId(candidate, &owner);
            if (owner == fixture->pid &&
                GetDlgItem(candidate, native_fixture::kEditId)) {
                fixture->root = candidate;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&fixture));
    ASSERT_TRUE(IsWindow(fixture.root));
    char fixtureHwnd[32]{};
    snprintf(
        fixtureHwnd, sizeof(fixtureHwnd), "0x%llX",
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(fixture.root)));

    const std::string eventName =
        "Local\\LvtMcpUiaLifetime_" +
        std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(GetTickCount64());
    wil::unique_event invalidated(CreateEventA(
        nullptr, TRUE, FALSE, eventName.c_str()));
    ASSERT_TRUE(invalidated);
    ScopedEnvironmentVariable invalidation(
        "LVT_TEST_UIA_LIFETIME_INVALIDATION_EVENT",
        eventName);

    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto connected = client.call_tool(
        "connect",
        json{{"hwnd", fixtureHwnd},
             {"mode", "uia"}});
    const auto session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);
    const auto visualConnected = client.call_tool(
        "connect",
        json{{"hwnd", fixtureHwnd},
             {"mode", "visual"}});
    const auto visualSession =
        visualConnected.value("session", "");
    ASSERT_FALSE(visualSession.empty())
        << visualConnected.dump(2);

    bool isError = false;
    const auto stale = client.call_tool(
        "get_editable_properties",
        json{{"session", session},
             {"element", "uia:99.99.99"}},
        &isError);
    EXPECT_TRUE(isError) << stale.dump(2);
    EXPECT_NE(
        stale.value("errorCode", ""),
        "typed_property_session_disconnected")
        << "a missing element in a live target is not ownership loss: "
        << stale.dump(2);

    SetEvent(invalidated.get());
    for (const auto& [tool, arguments] :
         std::vector<std::pair<std::string, json>>{
             {"type_text",
              json{{"session", session},
                   {"text", "must-not-type"}}},
             {"press_key",
              json{{"session", session},
                   {"text", "A"}}}}) {
        isError = false;
        const auto result =
            client.call_tool(tool, arguments, &isError);
        EXPECT_TRUE(isError) << result.dump(2);
        EXPECT_EQ(result.value("code", ""), "ownershipLost")
            << result.dump(2);
    }

    for (const auto& [waitSession, element] :
         std::vector<std::pair<std::string, std::string>>{
             {session, "uia:99.99.99"},
             {visualSession, "visual:e0"}}) {
        isError = false;
        const auto waiting = client.call_tool(
            "wait_for",
            json{{"session", waitSession},
                 {"element", element},
                 {"timeoutMs", 500}},
            &isError);
        EXPECT_TRUE(isError) << waiting.dump(2);
        EXPECT_EQ(waiting.value("code", ""), "ownershipLost")
            << waiting.dump(2);

        isError = false;
        const auto gone = client.call_tool(
            "wait_for",
            json{{"session", waitSession},
                 {"element", element},
                 {"gone", true},
                 {"timeoutMs", 500}},
            &isError);
        EXPECT_FALSE(isError) << gone.dump(2);
        EXPECT_TRUE(gone.value("ok", false))
            << gone.dump(2);
    }

    for (const auto& [tool, arguments] :
         std::vector<std::pair<std::string, json>>{
             {"type_text",
              json{{"session", visualSession},
                   {"text", "must-not-type"}}},
             {"press_key",
              json{{"session", visualSession},
                   {"text", "A"}}},
             {"window_action",
              json{{"session", visualSession},
                   {"action", "minimize"}}}}) {
        isError = false;
        const auto result =
            client.call_tool(tool, arguments, &isError);
        EXPECT_TRUE(isError) << result.dump(2);
        EXPECT_EQ(result.value("code", ""), "ownershipLost")
            << result.dump(2);
    }

}

TEST(McpUiaIdentity, TypedMutationsDetectOwnershipLossAfterInitialCheck) {
    const auto runMutation = [](const std::string& tool) {
        ManagedSampleProcess sample;
        ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
        struct FixtureWindow {
            DWORD pid;
            HWND root = nullptr;
        } fixture{sample.pid()};
        EnumWindows(
            [](HWND candidate, LPARAM parameter) -> BOOL {
                auto* fixture =
                    reinterpret_cast<FixtureWindow*>(parameter);
                DWORD owner = 0;
                GetWindowThreadProcessId(candidate, &owner);
                if (owner == fixture->pid &&
                    GetDlgItem(candidate, native_fixture::kEditId)) {
                    fixture->root = candidate;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&fixture));
        ASSERT_TRUE(IsWindow(fixture.root));
        char fixtureHwnd[32]{};
        snprintf(
            fixtureHwnd, sizeof(fixtureHwnd), "0x%llX",
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(fixture.root)));

        const std::string suffix =
            std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(GetTickCount64()) + "_" + tool;
        const std::string invalidationName =
            "Local\\LvtTypedInvalidation_" + suffix;
        const std::string gateBase =
            "Local\\LvtTypedMutation_" + suffix;
        wil::unique_event invalidated(CreateEventA(
            nullptr, TRUE, FALSE, invalidationName.c_str()));
        wil::unique_event entered(CreateEventA(
            nullptr, TRUE, FALSE,
            (gateBase + "-entered").c_str()));
        wil::unique_event release(CreateEventA(
            nullptr, TRUE, FALSE,
            (gateBase + "-release").c_str()));
        ASSERT_TRUE(invalidated);
        ASSERT_TRUE(entered);
        ASSERT_TRUE(release);
        ScopedEnvironmentVariable invalidation(
            "LVT_TEST_UIA_LIFETIME_INVALIDATION_EVENT",
            invalidationName);
        ScopedEnvironmentVariable mutationGate(
            "LVT_TEST_UIA_BEFORE_PROPERTY_MUTATION_GATE",
            gateBase);

        McpClient client(true);
        ASSERT_TRUE(client.started());
        ASSERT_TRUE(client.handshake());
        const auto connected = client.call_tool(
            "connect",
            json{{"hwnd", fixtureHwnd}, {"mode", "uia"}});
        const auto session = connected.value("session", "");
        ASSERT_FALSE(session.empty()) << connected.dump(2);
        const auto inputs = client.call_tool(
            "find_elements",
            json{{"session", session},
                 {"automationId",
                  std::to_string(native_fixture::kEditId)}});
        ASSERT_EQ(inputs["elements"].size(), 1u)
            << inputs.dump(2);
        const auto inputKey =
            inputs["elements"][0].value("key", "");
        bool isError = false;
        const auto properties = client.call_tool(
            "get_editable_properties",
            json{{"session", session},
                 {"element", inputKey}},
            &isError);
        ASSERT_FALSE(isError) << properties.dump(2);
        const auto* value = find_property_descriptor(
            properties, "Value.Value");
        ASSERT_NE(value, nullptr);
        json arguments{
            {"session", session},
            {"element", inputKey},
            {"descriptorId",
             value->value("descriptorId", "")},
        };
        if (tool == "set_property")
            arguments["value"] = "must-not-set";

        const int request = client.send_request(
            "tools/call",
            json{{"name", tool},
                 {"arguments", arguments}});
        ASSERT_EQ(
            WaitForSingleObject(entered.get(), 10000),
            WAIT_OBJECT_0)
            << tool << " did not reach the provider mutation gate";
        SetEvent(invalidated.get());
        SetEvent(release.get());

        const auto response = client.await_response(request);
        ASSERT_TRUE(response.contains("result"))
            << response.dump(2);
        EXPECT_TRUE(
            response["result"].value("isError", false))
            << response.dump(2);
        json error;
        for (const auto& block :
             response["result"].value(
                 "content", json::array())) {
            if (block.value("type", "") == "text") {
                error = json::parse(
                    block.value("text", "{}"),
                    nullptr, false);
                break;
            }
        }
        ASSERT_FALSE(error.is_discarded())
            << response.dump(2);
        expect_typed_property_session_disconnected(error);
    };

    runMutation("set_property");
    runMutation("clear_property");
}

TEST(McpUiaIdentity, NonRecycleTypedReferenceResolutionRacesAreStructured) {
    const auto runRace = [](const std::string& tool) {
        ManagedSampleProcess sample;
        ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
        struct FixtureWindow {
            DWORD pid;
            HWND root = nullptr;
        } fixture{sample.pid()};
        EnumWindows(
            [](HWND candidate, LPARAM parameter) -> BOOL {
                auto* fixture =
                    reinterpret_cast<FixtureWindow*>(parameter);
                DWORD owner = 0;
                GetWindowThreadProcessId(candidate, &owner);
                if (owner == fixture->pid &&
                    GetDlgItem(candidate, native_fixture::kEditId)) {
                    fixture->root = candidate;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&fixture));
        ASSERT_TRUE(IsWindow(fixture.root));
        const HWND edit = GetDlgItem(
            fixture.root, native_fixture::kEditId);
        ASSERT_TRUE(IsWindow(edit));
        char editHwnd[32]{};
        snprintf(
            editHwnd, sizeof(editHwnd), "0x%llX",
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(edit)));

        const std::string gateBase =
            "Local\\LvtTypedReference_" +
            std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(GetTickCount64()) + "_" + tool;
        wil::unique_event entered(CreateEventA(
            nullptr, TRUE, FALSE,
            (gateBase + "-entered").c_str()));
        wil::unique_event release(CreateEventA(
            nullptr, TRUE, FALSE,
            (gateBase + "-release").c_str()));
        ASSERT_TRUE(entered);
        ASSERT_TRUE(release);
        ScopedEnvironmentVariable referenceGate(
            "LVT_TEST_UIA_BEFORE_PROPERTY_REFERENCE_GATE",
            gateBase);

        McpClient client(true);
        ASSERT_TRUE(client.started());
        ASSERT_TRUE(client.handshake());
        const auto connected = client.call_tool(
            "connect",
            json{{"hwnd", editHwnd}, {"mode", "uia"}});
        const auto session = connected.value("session", "");
        ASSERT_FALSE(session.empty()) << connected.dump(2);
        const auto tree = client.call_tool(
            "get_uia_tree", json{{"session", session}});
        ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
        const auto runtimeId =
            tree["root"]
                .value("properties", json::object())
                .value("RuntimeId", "");
        ASSERT_FALSE(runtimeId.empty()) << tree.dump(2);
        const auto inputRef = "uia:" + runtimeId;

        bool isError = false;
        const auto properties = client.call_tool(
            "get_editable_properties",
            json{{"session", session},
                 {"element", inputRef}},
            &isError);
        ASSERT_FALSE(isError) << properties.dump(2);
        const auto* value = find_property_descriptor(
            properties, "Value.Value");
        ASSERT_NE(value, nullptr);
        json arguments{
            {"session", session},
            {"element", inputRef},
        };
        if (tool != "get_editable_properties") {
            arguments["descriptorId"] =
                value->value("descriptorId", "");
        }
        if (tool == "set_property")
            arguments["value"] = "must-not-set";

        const int request = client.send_request(
            "tools/call",
            json{{"name", tool},
                 {"arguments", arguments}});
        ASSERT_EQ(
            WaitForSingleObject(entered.get(), 10000),
            WAIT_OBJECT_0)
            << tool << " did not reach reference resolution";

        sample.stop();
        SetEvent(release.get());

        const auto response = client.await_response(request);
        ASSERT_TRUE(response.contains("result"))
            << response.dump(2);
        EXPECT_TRUE(
            response["result"].value("isError", false))
            << response.dump(2);
        json error;
        for (const auto& block :
             response["result"].value(
                 "content", json::array())) {
            if (block.value("type", "") == "text") {
                error = json::parse(
                    block.value("text", "{}"),
                    nullptr, false);
                break;
            }
        }
        ASSERT_FALSE(error.is_discarded())
            << response.dump(2);
        expect_typed_property_session_disconnected(error);
    };

    runRace("get_editable_properties");
    runRace("clear_property");
}

TEST(
    McpUiaIdentity,
    ExactRecycledHwndTypedReferenceResolutionRaceIsStructured) {
    if (lvt::test_support::exact_hwnd_recycle_search_suppressed()) {
        GTEST_SKIP()
            << lvt::test_support::exact_hwnd_recycle_unavailable_reason();
    }

    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    struct FixtureWindow {
        DWORD pid;
        HWND root = nullptr;
    } fixture{sample.pid()};
    EnumWindows(
        [](HWND candidate, LPARAM parameter) -> BOOL {
            auto* fixture =
                reinterpret_cast<FixtureWindow*>(parameter);
            DWORD owner = 0;
            GetWindowThreadProcessId(candidate, &owner);
            if (owner == fixture->pid &&
                GetDlgItem(candidate, native_fixture::kEditId)) {
                fixture->root = candidate;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&fixture));
    ASSERT_TRUE(IsWindow(fixture.root));
    const HWND edit = GetDlgItem(
        fixture.root, native_fixture::kEditId);
    ASSERT_TRUE(IsWindow(edit));
    char editHwnd[32]{};
    snprintf(
        editHwnd, sizeof(editHwnd), "0x%llX",
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(edit)));

    const std::string gateBase =
        "Local\\LvtTypedReferenceExact_" +
        std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(GetTickCount64());
    wil::unique_event entered(CreateEventA(
        nullptr, TRUE, FALSE,
        (gateBase + "-entered").c_str()));
    wil::unique_event release(CreateEventA(
        nullptr, TRUE, FALSE,
        (gateBase + "-release").c_str()));
    ASSERT_TRUE(entered);
    ASSERT_TRUE(release);
    ScopedEnvironmentVariable referenceGate(
        "LVT_TEST_UIA_BEFORE_PROPERTY_REFERENCE_GATE",
        gateBase);

    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto connected = client.call_tool(
        "connect",
        json{{"hwnd", editHwnd}, {"mode", "uia"}});
    const auto session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);
    const auto tree = client.call_tool(
        "get_uia_tree", json{{"session", session}});
    ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
    const auto runtimeId =
        tree["root"]
            .value("properties", json::object())
            .value("RuntimeId", "");
    ASSERT_FALSE(runtimeId.empty()) << tree.dump(2);
    const auto inputRef = "uia:" + runtimeId;

    bool isError = false;
    const auto properties = client.call_tool(
        "get_editable_properties",
        json{{"session", session},
             {"element", inputRef}},
        &isError);
    ASSERT_FALSE(isError) << properties.dump(2);
    const auto* value = find_property_descriptor(
        properties, "Value.Value");
    ASSERT_NE(value, nullptr);

    const int request = client.send_request(
        "tools/call",
        json{
            {"name", "set_property"},
            {"arguments",
             json{{"session", session},
                  {"element", inputRef},
                  {"descriptorId",
                   value->value("descriptorId", "")},
                  {"value", "must-not-set"}}}});
    lvt::test_support::ScopedEventSignal releaseOnExit(
        release.get());
    ASSERT_EQ(
        WaitForSingleObject(entered.get(), 10000),
        WAIT_OBJECT_0)
        << "set_property did not reach reference resolution";

    const auto recycled =
        lvt::test_support::recycle_event_child_exact(
            fixture.root, edit, native_fixture::kEditId);
    releaseOnExit.signal();
    const auto response = client.await_response(request);
    ASSERT_TRUE(response.contains("result"))
        << response.dump(2);

    if (lvt::test_support::exact_hwnd_recycle_is_unavailable(
            recycled)) {
        client.shutdown();
        sample.stop();
        GTEST_SKIP() << recycled.reason;
    }
    ASSERT_EQ(
        recycled.outcome,
        lvt::test_support::ExactHwndRecycleOutcome::achieved)
        << recycled.reason;
    ASSERT_EQ(recycled.replacement, edit);

    EXPECT_TRUE(
        response["result"].value("isError", false))
        << response.dump(2);
    json error;
    for (const auto& block :
         response["result"].value(
             "content", json::array())) {
        if (block.value("type", "") == "text") {
            error = json::parse(
                block.value("text", "{}"),
                nullptr, false);
            break;
        }
    }
    ASSERT_FALSE(error.is_discarded())
        << response.dump(2);
    expect_typed_property_session_disconnected(error);
}

TEST(McpUiaIdentity, AltTabSequenceRechecksForegroundBeforeDelete) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto uiaStats =
        plugin_stats_path("uia-alt-tab-sequence");
    const auto visualStats =
        plugin_stats_path("visual-alt-tab-sequence");
    std::error_code ec;
    fs::remove(uiaStats, ec);
    fs::remove(visualStats, ec);
    ScopedEnvironmentVariable uiaForeground(
        "LVT_TEST_UIA_FOREGROUND_SUCCESS", "1");
    ScopedEnvironmentVariable uiaGate(
        "LVT_TEST_UIA_AFTER_FOREGROUND_GATE",
        "Local\\LvtUnusedForegroundGate");
    ScopedEnvironmentVariable uiaOutput(
        "LVT_TEST_UIA_SEND_INPUT_STATS", uiaStats.string());
    ScopedEnvironmentVariable uiaSuppress(
        "LVT_TEST_UIA_SUPPRESS_SEND_INPUT", "1");
    ScopedEnvironmentVariable uiaSteal(
        "LVT_TEST_UIA_FOREGROUND_STEAL_AT", "2");
    ScopedEnvironmentVariable visualForeground(
        "LVT_TEST_VISUAL_FOREGROUND_SUCCESS", "1");
    ScopedEnvironmentVariable visualOutput(
        "LVT_TEST_VISUAL_SEND_INPUT_STATS",
        visualStats.string());
    ScopedEnvironmentVariable visualSuppress(
        "LVT_TEST_VISUAL_SUPPRESS_SEND_INPUT", "1");
    ScopedEnvironmentVariable visualSteal(
        "LVT_TEST_VISUAL_FOREGROUND_STEAL_AT", "2");

    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto uiaConnected = client.call_tool(
        "connect",
        json{{"hwnd", sample.hwnd_string()}, {"mode", "uia"}});
    const auto visualConnected = client.call_tool(
        "connect",
        json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    const auto uiaSession =
        uiaConnected.value("session", "");
    const auto visualSession =
        visualConnected.value("session", "");
    ASSERT_FALSE(uiaSession.empty()) << uiaConnected.dump(2);
    ASSERT_FALSE(visualSession.empty())
        << visualConnected.dump(2);

    ShowWindow(sample.hwnd(), SW_MINIMIZE);
    Sleep(100);
    ASSERT_TRUE(IsIconic(sample.hwnd()));
    for (const auto& session :
         {uiaSession, visualSession}) {
        bool isError = false;
        const auto result = client.call_tool(
            "press_key",
            json{{"session", session},
                 {"text", "Alt+Tab;Delete"}},
            &isError);
        EXPECT_FALSE(isError) << result.dump(2);
    }
    EXPECT_FALSE(IsIconic(sample.hwnd()))
        << "unconditional visual activation must restore a minimized root";

    for (const auto& statsPath :
         {uiaStats, visualStats}) {
        const auto stats = read_plugin_stats(statsPath);
        EXPECT_EQ(
            count_plugin_stats(stats, "press-key"), 2u)
            << statsPath.string();
        EXPECT_EQ(
            count_plugin_stats(stats, "reactivate"), 1u)
            << "the second chord must re-activate after Alt+Tab: "
            << statsPath.string();
    }
    const auto visualActivationStats =
        read_plugin_stats(visualStats);
    EXPECT_EQ(
        count_plugin_stats(
            visualActivationStats, "activate"),
        2u)
        << "visual mode must activate initially even when already foreground, "
           "then activate again after the simulated Alt+Tab";
    fs::remove(uiaStats, ec);
    fs::remove(visualStats, ec);
}

TEST(McpUiaIdentity, VisualMinimizedClickAlwaysRunsActivation) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto statsPath =
        plugin_stats_path("visual-minimized-click");
    std::error_code ec;
    fs::remove(statsPath, ec);
    ScopedEnvironmentVariable visualForeground(
        "LVT_TEST_VISUAL_FOREGROUND_SUCCESS", "1");
    ScopedEnvironmentVariable visualOutput(
        "LVT_TEST_VISUAL_SEND_INPUT_STATS",
        statsPath.string());
    ScopedEnvironmentVariable visualSuppress(
        "LVT_TEST_VISUAL_SUPPRESS_SEND_INPUT", "1");

    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto connected = client.call_tool(
        "connect",
        json{{"hwnd", sample.hwnd_string()},
             {"mode", "visual"}});
    const auto session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);
    const auto tree = client.call_tool(
        "get_visual_tree", json{{"session", session}});
    ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
    const auto rootRef =
        tree["root"].value("ref", "");
    ASSERT_FALSE(rootRef.empty()) << tree.dump(2);

    ShowWindow(sample.hwnd(), SW_MINIMIZE);
    ASSERT_TRUE(IsIconic(sample.hwnd()));
    bool isError = false;
    const auto clicked = client.call_tool(
        "click",
        json{{"session", session},
             {"element", rootRef}},
        &isError);
    EXPECT_FALSE(isError) << clicked.dump(2);
    EXPECT_FALSE(IsIconic(sample.hwnd()));

    const auto stats = read_plugin_stats(statsPath);
    EXPECT_EQ(count_plugin_stats(stats, "activate"), 1u);
    EXPECT_EQ(count_plugin_stats(stats, "click"), 1u);
    fs::remove(statsPath, ec);
}

TEST(McpUiaIdentity, WindowPatternCloseSucceedsWhenTargetDisappears) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const HWND target = sample.hwnd();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto connected = client.call_tool(
        "connect",
        json{{"hwnd", sample.hwnd_string()}, {"mode", "uia"}});
    const auto session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);
    const auto tree = client.call_tool(
        "get_uia_tree", json{{"session", session}});
    ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
    const auto runtimeId =
        tree["root"]
            .value("properties", json::object())
            .value("RuntimeId", "");
    ASSERT_FALSE(runtimeId.empty()) << tree.dump(2);

    bool isError = false;
    const auto closed = client.call_tool(
        "window_action",
        json{{"session", session},
             {"element", "uia:" + runtimeId},
             {"action", "close"}},
        &isError);
    EXPECT_FALSE(isError) << closed.dump(2);
    EXPECT_TRUE(closed.value("ok", false)) << closed.dump(2);
    EXPECT_EQ(closed.value("method", ""), "WindowPattern.Close")
        << closed.dump(2);
    const auto deadline = GetTickCount64() + 5000;
    while (IsWindow(target) && GetTickCount64() < deadline)
        Sleep(20);
    EXPECT_FALSE(IsWindow(target));
}

TEST(McpUiaIdentity, ExactRecycledHwndRejectsOldSessionAndActions) {
    if (lvt::test_support::exact_hwnd_recycle_search_suppressed()) {
        GTEST_SKIP()
            << lvt::test_support::exact_hwnd_recycle_unavailable_reason();
    }
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    struct FixtureWindow {
        DWORD pid;
        HWND root = nullptr;
    } fixture{sample.pid()};
    EnumWindows(
        [](HWND candidate, LPARAM parameter) -> BOOL {
            auto* fixture =
                reinterpret_cast<FixtureWindow*>(parameter);
            DWORD owner = 0;
            GetWindowThreadProcessId(candidate, &owner);
            if (owner == fixture->pid &&
                GetDlgItem(candidate, native_fixture::kEditId)) {
                fixture->root = candidate;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&fixture));
    ASSERT_TRUE(IsWindow(fixture.root));

    const HWND original = GetDlgItem(
        fixture.root, native_fixture::kEditId);
    ASSERT_TRUE(IsWindow(original));
    char originalText[32]{};
    snprintf(
        originalText, sizeof(originalText), "0x%llX",
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(original)));

    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto connected = client.call_tool(
        "connect",
        json{{"hwnd", originalText}, {"mode", "uia"}});
    const auto session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);
    const auto visualConnected = client.call_tool(
        "connect",
        json{{"hwnd", originalText}, {"mode", "visual"}});
    const auto visualSession =
        visualConnected.value("session", "");
    ASSERT_FALSE(visualSession.empty())
        << visualConnected.dump(2);
    const auto initialTree = client.call_tool(
        "get_uia_tree", json{{"session", session}});
    ASSERT_TRUE(initialTree.contains("root"))
        << initialTree.dump(2);
    const auto runtimeId =
        initialTree["root"]
            .value("properties", json::object())
            .value("RuntimeId", "");
    ASSERT_FALSE(runtimeId.empty()) << initialTree.dump(2);
    const auto inputRef = "uia:" + runtimeId;
    bool isError = false;
    const auto properties = client.call_tool(
        "get_editable_properties",
        json{{"session", session},
             {"element", inputRef}},
        &isError);
    ASSERT_FALSE(isError) << properties.dump(2);
    const auto* value = find_property_descriptor(
        properties, "Value.Value");
    ASSERT_NE(value, nullptr);
    const auto descriptorId =
        value->value("descriptorId", "");

    const auto recycled =
        lvt::test_support::recycle_event_child_exact(
            fixture.root, original,
            native_fixture::kEditId);
    if (lvt::test_support::exact_hwnd_recycle_is_unavailable(
            recycled)) {
        client.shutdown();
        sample.stop();
        GTEST_SKIP() << recycled.reason;
    }
    ASSERT_EQ(
        recycled.outcome,
        lvt::test_support::ExactHwndRecycleOutcome::achieved)
        << recycled.reason;
    ASSERT_EQ(recycled.replacement, original);

    bool treeError = false;
    const auto tree = client.call_tool(
        "get_uia_tree",
        json{{"session", session}}, &treeError);
    EXPECT_TRUE(treeError) << tree.dump(2);
    EXPECT_NE(
        tree.dump().find("ownershipLost"),
        std::string::npos)
        << tree.dump(2);

    for (const auto& [tool, text] :
         std::vector<std::pair<std::string, std::string>>{
             {"type_text", "must-not-type"},
             {"press_key", "A"}}) {
        bool actionError = false;
        const auto action = client.call_tool(
            tool,
            json{{"session", session}, {"text", text}},
            &actionError);
        EXPECT_TRUE(actionError) << action.dump(2);
        EXPECT_EQ(
            action.value("code", ""),
            "ownershipLost")
            << action.dump(2);
    }

    for (const auto& [tool, arguments] :
         std::vector<std::pair<std::string, json>>{
             {"type_text",
              json{{"session", visualSession},
                   {"text", "must-not-type"}}},
             {"press_key",
              json{{"session", visualSession},
                   {"text", "A"}}},
             {"window_action",
              json{{"session", visualSession},
                   {"action", "minimize"}}}}) {
        bool actionError = false;
        const auto action = client.call_tool(
            tool, arguments, &actionError);
        EXPECT_TRUE(actionError) << action.dump(2);
        EXPECT_EQ(
            action.value("code", ""),
            "ownershipLost")
            << action.dump(2);
    }

    for (const auto& [tool, arguments] :
         std::vector<std::pair<std::string, json>>{
             {"get_editable_properties",
              json{{"session", session},
                   {"element", inputRef}}},
             {"set_property",
              json{{"session", session},
                   {"element", inputRef},
                   {"descriptorId", descriptorId},
                   {"value", "must-not-set"}}},
             {"clear_property",
              json{{"session", session},
                   {"element", inputRef},
                   {"descriptorId", descriptorId}}}}) {
        isError = false;
        const auto result = client.call_tool(
            tool, arguments, &isError);
        EXPECT_TRUE(isError) << result.dump(2);
        expect_typed_property_session_disconnected(result);
    }
}

TEST(McpPluginPersistent, SharesRegistryConnectionAcrossSessions) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto statsPath = plugin_stats_path("shared");
    std::error_code ec;
    fs::remove(statsPath, ec);

    auto client = start_plugin_mcp(
        LVT_FAKE_PLUGIN_V2_DIR, statsPath, "0", "0", "0", "0");
    ASSERT_TRUE(client->started());
    ASSERT_TRUE(client->handshake());

    const auto connect = [&](const char* filter) {
        const auto result = client->call_tool(
            "connect",
            json{{"hwnd", sample.hwnd_string()},
                 {"title", filter},
                 {"mode", "visual"}});
        return result.value("session", "");
    };
    const std::string firstSession = connect("first-filter");
    const std::string secondSession = connect("second-filter");
    ASSERT_FALSE(firstSession.empty());
    ASSERT_FALSE(secondSession.empty());

    const auto readAndCheck =
        [&](const std::string& session, const char* expectedFilter) {
            const auto tree = client->call_tool(
                "get_visual_tree", json{{"session", session}});
            ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
            const auto* node = find_fake_plugin_node(tree["root"]);
            ASSERT_NE(node, nullptr);
            EXPECT_EQ(
                node->value("properties", json::object()).value("filter", ""),
                expectedFilter);
        };
    readAndCheck(firstSession, "first-filter");
    readAndCheck(secondSession, "second-filter");
    EXPECT_EQ(
        count_plugin_stats(read_plugin_stats(statsPath), "open "), 1u);

    bool isError = false;
    client->call_tool(
        "disconnect", json{{"session", firstSession}}, &isError);
    ASSERT_FALSE(isError);
    EXPECT_EQ(
        count_plugin_stats(read_plugin_stats(statsPath), "close"), 0u);

    readAndCheck(secondSession, "second-filter");
    client->call_tool(
        "disconnect", json{{"session", secondSession}}, &isError);
    ASSERT_FALSE(isError);

    const auto stats = read_plugin_stats(statsPath);
    EXPECT_EQ(count_plugin_stats(stats, "open "), 1u);
    EXPECT_EQ(count_plugin_stats(stats, "get "), 3u);
    EXPECT_EQ(count_plugin_stats(stats, "free"), 3u);
    EXPECT_EQ(count_plugin_stats(stats, "poll"), 3u)
        << "each successful plugin snapshot must have one post-success poll";
    EXPECT_EQ(count_plugin_stats(stats, "events_free"), 0u);
    EXPECT_EQ(count_plugin_stats(stats, "close"), 1u);

    client->shutdown();
    fs::remove(statsPath, ec);
}

TEST(McpPluginPersistent, DisconnectDuringDetectionNeverFallsBackOneShot) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto statsPath = plugin_stats_path("disconnect-detection");
    std::error_code ec;
    fs::remove(statsPath, ec);

    auto client = start_plugin_mcp(
        LVT_FAKE_PLUGIN_V2_DIR, statsPath, "0", "0", "0", "0",
        "2", "500");
    ASSERT_TRUE(client->started());
    ASSERT_TRUE(client->handshake());
    const auto connected = client->call_tool(
        "connect",
        json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    const std::string session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);

    const int readId = client->send_request(
        "tools/call",
        json{{"name", "get_visual_tree"},
             {"arguments", json{{"session", session}}}});
    const auto detectionDeadline = GetTickCount64() + 5000;
    while (GetTickCount64() < detectionDeadline) {
        if (count_plugin_stats(read_plugin_stats(statsPath), "detect ") >= 2)
            break;
        Sleep(10);
    }
    ASSERT_GE(
        count_plugin_stats(read_plugin_stats(statsPath), "detect "), 2u);

    const int disconnectId = client->send_request(
        "tools/call",
        json{{"name", "disconnect"},
             {"arguments", json{{"session", session}}}});
    const auto readResponse = client->await_response(readId);
    const auto disconnectResponse = client->await_response(disconnectId);
    ASSERT_TRUE(readResponse.contains("result")) << readResponse.dump(2);
    ASSERT_TRUE(disconnectResponse.contains("result"))
        << disconnectResponse.dump(2);
    EXPECT_FALSE(readResponse["result"].value("isError", false));
    EXPECT_FALSE(disconnectResponse["result"].value("isError", false));

    const auto stats = read_plugin_stats(statsPath);
    EXPECT_EQ(count_plugin_stats(stats, "enrich "), 0u);
    EXPECT_EQ(count_plugin_stats(stats, "open "), 1u);
    EXPECT_EQ(count_plugin_stats(stats, "get "), 1u);
    EXPECT_EQ(count_plugin_stats(stats, "close"), 1u);

    client->shutdown();
    fs::remove(statsPath, ec);
}

TEST(McpPluginPersistent, PreservesDetectingPluginIdentityAndAlias) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto statsPath = plugin_stats_path("identity");
    std::error_code ec;
    fs::remove(statsPath, ec);

    auto client = start_plugin_mcp(
        LVT_FAKE_PLUGIN_IDENTITY_DIR, statsPath, "0", "0", "0", "0");
    ASSERT_TRUE(client->started());
    ASSERT_TRUE(client->handshake());
    const auto connected = client->call_tool(
        "connect",
        json{{"hwnd", sample.hwnd_string()},
             {"title", "identity-filter"},
             {"mode", "visual"}});
    const std::string session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);
    ASSERT_TRUE(connected.contains("frameworks"));
    EXPECT_EQ(
        std::count(
            connected["frameworks"].begin(), connected["frameworks"].end(),
            "shared-alias test"),
        2);

    const auto tree =
        client->call_tool("get_visual_tree", json{{"session", session}});
    ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
    std::set<std::string> pluginNames;
    std::vector<const json*> elements;
    collect_json_elements(tree["root"], elements);
    for (const auto* element : elements) {
        if (element->value("type", "") != "FakePluginNode")
            continue;
        EXPECT_EQ(element->value("framework", ""), "shared-alias");
        const auto properties =
            element->value("properties", json::object());
        EXPECT_EQ(properties.value("filter", ""), "identity-filter");
        pluginNames.insert(properties.value("plugin", ""));
    }
    EXPECT_EQ(
        pluginNames,
        (std::set<std::string>{"fake-plugin-a", "fake-plugin-b"}));

    bool isError = false;
    client->call_tool(
        "disconnect", json{{"session", session}}, &isError);
    ASSERT_FALSE(isError);
    const auto stats = read_plugin_stats(statsPath);
    EXPECT_EQ(count_plugin_stats(stats, "open "), 2u);
    EXPECT_EQ(count_plugin_stats(stats, "get "), 2u);
    EXPECT_EQ(count_plugin_stats(stats, "close"), 2u);

    client->shutdown();
    fs::remove(statsPath, ec);
}

void verify_legacy_plugin_mcp_fallback(
    const fs::path& pluginDirectory, const std::string& variant) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto statsPath = plugin_stats_path(variant);
    std::error_code ec;
    fs::remove(statsPath, ec);

    auto client =
        start_plugin_mcp(pluginDirectory, statsPath, "0", "0", "0", "0");
    ASSERT_TRUE(client->started());
    ASSERT_TRUE(client->handshake());
    const auto connected = client->call_tool(
        "connect",
        json{{"hwnd", sample.hwnd_string()},
             {"title", "legacy-filter"},
             {"mode", "visual"}});
    const std::string session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);

    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto tree = client->call_tool(
            "get_visual_tree", json{{"session", session}});
        ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
        const auto* node = find_fake_plugin_node(tree["root"]);
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(
            node->value("properties", json::object()).value("source", ""),
            "one-shot");
        EXPECT_EQ(
            node->value("properties", json::object()).value("filter", ""),
            "legacy-filter");
    }

    bool isError = false;
    client->call_tool(
        "disconnect", json{{"session", session}}, &isError);
    EXPECT_FALSE(isError);
    const auto stats = read_plugin_stats(statsPath);
    EXPECT_EQ(count_plugin_stats(stats, "enrich "), 2u);
    EXPECT_EQ(count_plugin_stats(stats, "free"), 2u);
    EXPECT_EQ(count_plugin_stats(stats, "open "), 0u);
    EXPECT_EQ(count_plugin_stats(stats, "get "), 0u);
    EXPECT_EQ(count_plugin_stats(stats, "poll"), 0u);
    EXPECT_EQ(count_plugin_stats(stats, "close"), 0u);

    client->shutdown();
    fs::remove(statsPath, ec);
}

TEST(McpPluginPersistent, V1AndPartialV2RemainOneShot) {
    verify_legacy_plugin_mcp_fallback(
        LVT_FAKE_PLUGIN_V1_DIR, "legacy");
    verify_legacy_plugin_mcp_fallback(
        LVT_FAKE_PLUGIN_PARTIAL_DIR, "partial");
}

class ManagedSampleUiBlock {
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

    ~ManagedSampleUiBlock() {
        release();
    }

private:
    wil::unique_handle trigger_;
    wil::unique_handle entered_;
    wil::unique_handle release_;
    bool active_ = false;
};

int count_managed_tap_starts(const wchar_t* logName, DWORD pid) {
    wchar_t tempPath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempPath) == 0)
        return 0;
    std::ifstream log(std::wstring(tempPath) + logName, std::ios::binary);
    const std::string pidMarker = "[pid=" + std::to_string(pid) + " ";
    int count = 0;
    std::string line;
    while (std::getline(log, line)) {
        if (line.find(pidMarker) != std::string::npos &&
            line.find("Persistent managed TAP worker starting") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

const json* find_managed_named_element(const json& root, const std::string& name) {
    std::vector<const json*> elements;
    collect_json_elements(root, elements);
    for (const auto* element : elements) {
        if (element->value("properties", json::object()).value("name", "") == name)
            return element;
    }
    return nullptr;
}

void verify_managed_mcp_connection(
    const fs::path& executable, const std::string& elementName,
    const std::string& keyPrefix, const wchar_t* logName) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(executable));
    const int startsBefore = count_managed_tap_starts(logName, sample.pid());

    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    auto connected = client.call_tool(
        "connect", json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    const std::string session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);

    auto first = client.call_tool("get_visual_tree", json{{"session", session}});
    auto second = client.call_tool("get_visual_tree", json{{"session", session}});
    ASSERT_TRUE(first.contains("root")) << first.dump(2);
    ASSERT_TRUE(second.contains("root")) << second.dump(2);
    const json* firstElement = find_managed_named_element(first["root"], elementName);
    const json* secondElement = find_managed_named_element(second["root"], elementName);
    ASSERT_NE(firstElement, nullptr);
    ASSERT_NE(secondElement, nullptr);
    const std::string firstKey = firstElement->value("key", "");
    const std::string firstHandle =
        firstElement->value("properties", json::object()).value("managedHandle", "");
    EXPECT_EQ(firstKey.rfind(keyPrefix, 0), 0u);
    EXPECT_FALSE(firstHandle.empty());
    EXPECT_EQ(secondElement->value("key", ""), firstKey);
    EXPECT_EQ(secondElement->value("properties", json::object()).value("managedHandle", ""),
              firstHandle);

    bool isError = false;
    auto disconnected =
        client.call_tool("disconnect", json{{"session", session}}, &isError);
    ASSERT_FALSE(isError) << disconnected.dump(2);
    EXPECT_EQ(count_managed_tap_starts(logName, sample.pid()) - startsBefore, 1)
        << "two MCP tree reads must reuse exactly one managed TAP injection";

    connected = client.call_tool(
        "connect", json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    const std::string reconnectedSession = connected.value("session", "");
    ASSERT_FALSE(reconnectedSession.empty()) << connected.dump(2);
    auto third =
        client.call_tool("get_visual_tree", json{{"session", reconnectedSession}});
    ASSERT_TRUE(third.contains("root")) << third.dump(2);
    const json* thirdElement = find_managed_named_element(third["root"], elementName);
    ASSERT_NE(thirdElement, nullptr);
    EXPECT_EQ(thirdElement->value("key", ""), firstKey);
    EXPECT_EQ(thirdElement->value("properties", json::object()).value("managedHandle", ""),
              firstHandle);
    client.call_tool("disconnect", json{{"session", reconnectedSession}}, &isError);
    ASSERT_FALSE(isError);
    EXPECT_EQ(count_managed_tap_starts(logName, sample.pid()) - startsBefore, 2)
        << "disconnect must tear down cleanly so a new MCP session can reconnect";

    connected = client.call_tool(
        "connect", json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    const std::string brokenPipeSession = connected.value("session", "");
    ASSERT_FALSE(brokenPipeSession.empty()) << connected.dump(2);
    auto beforeBreak =
        client.call_tool("get_visual_tree", json{{"session", brokenPipeSession}});
    ASSERT_TRUE(beforeBreak.contains("root")) << beforeBreak.dump(2);
    client.terminate_server();

    McpClient recovered(false);
    ASSERT_TRUE(recovered.started());
    ASSERT_TRUE(recovered.handshake());
    connected = recovered.call_tool(
        "connect", json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    const std::string recoveredSession = connected.value("session", "");
    ASSERT_FALSE(recoveredSession.empty()) << connected.dump(2);
    bool recoveredPropertyError = false;
    auto recoveredProperties = recovered.call_tool(
        "get_editable_properties",
        json{{"session", recoveredSession}, {"element", firstKey}},
        &recoveredPropertyError);
    ASSERT_FALSE(recoveredPropertyError) << recoveredProperties.dump(2);
    EXPECT_FALSE(
        recoveredProperties.value("descriptors", json::array()).empty())
        << "a broken-pipe reconnect must hydrate the stable identity map once";
    auto afterBreak =
        recovered.call_tool("get_visual_tree", json{{"session", recoveredSession}});
    ASSERT_TRUE(afterBreak.contains("root")) << afterBreak.dump(2);
    const json* recoveredElement =
        find_managed_named_element(afterBreak["root"], elementName);
    ASSERT_NE(recoveredElement, nullptr);
    EXPECT_EQ(recoveredElement->value("key", ""), firstKey);
    EXPECT_EQ(
        recoveredElement->value("properties", json::object()).value("managedHandle", ""),
        firstHandle);
    recovered.call_tool("disconnect", json{{"session", recoveredSession}}, &isError);
    ASSERT_FALSE(isError);
    EXPECT_EQ(count_managed_tap_starts(logName, sample.pid()) - startsBefore, 4)
        << "a broken MCP transport must let the managed server exit and reconnect";
}

// The sample app is the target for anything that needs known AutomationIds.
class McpSampleFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        const fs::path exe = WINUI3_SAMPLE_EXE_PATH;
        if (!fs::exists(exe)) {
            s_skip_reason = "the WinUI3 sample app is not available at " + exe.string();
            return;
        }
        const auto fixtureMachine = pe_machine(exe);
        if (fixtureMachine != IMAGE_FILE_MACHINE_UNKNOWN &&
            test_machine() != IMAGE_FILE_MACHINE_UNKNOWN &&
            fixtureMachine != test_machine()) {
            s_skip_reason =
                "WinUI3 fixture architecture mismatch: tests are " +
                std::string(machine_name(test_machine())) + ", fixture is " +
                machine_name(fixtureMachine);
            return;
        }
        STARTUPINFOA si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        std::string cmd = exe.string();
        SetEnvironmentVariableA("LVT_TEST_SECONDARY_WINDOW", "1");
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            exe.parent_path().string().c_str(), &si, &pi)) {
            SetEnvironmentVariableA("LVT_TEST_SECONDARY_WINDOW", nullptr);
            s_skip_reason =
                "failed to launch the WinUI3 sample app (error " +
                std::to_string(GetLastError()) + ")";
            return;
        }
        SetEnvironmentVariableA("LVT_TEST_SECONDARY_WINDOW", nullptr);
        s_process.reset(pi.hProcess);
        s_thread.reset(pi.hThread);
        WaitForInputIdle(s_process.get(), 10000);

        // Match on the process we started, not just the title. A leftover
        // instance from an earlier run has the same title, and binding to one
        // makes the whole suite depend on a window this fixture does not own —
        // which then disappears when that other run's teardown kills it.
        struct Search {
            DWORD pid;
            HWND found;
        } search{pi.dwProcessId, nullptr};

        for (int attempt = 0; attempt < 20 && !s_hwnd; ++attempt) {
            EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                auto* state = reinterpret_cast<Search*>(lParam);
                DWORD owner = 0;
                GetWindowThreadProcessId(hwnd, &owner);
                if (owner != state->pid || !IsWindowVisible(hwnd))
                    return TRUE;
                char title[256];
                GetWindowTextA(hwnd, title, sizeof(title));
                if (!strstr(title, "LVT WinUI3 Sample"))
                    return TRUE;
                state->found = hwnd;
                return FALSE;
            }, reinterpret_cast<LPARAM>(&search));
            s_hwnd = search.found;
            if (!s_hwnd)
                Sleep(500);
        }
        if (!s_hwnd)
            s_skip_reason = "the WinUI3 sample app did not create its test window";
    }

    static void TearDownTestSuite() {
        if (s_process)
            TerminateProcess(s_process.get(), 0);
        s_process.reset();
        s_thread.reset();
        s_hwnd = nullptr;
    }

    void SetUp() override {
        if (!s_hwnd)
            GTEST_SKIP() << (s_skip_reason.empty()
                ? "the WinUI3 sample app is not available"
                : s_skip_reason);
        if (!IsWindow(s_hwnd))
            GTEST_SKIP() << "the WinUI3 sample app's window closed during the run";
    }

    void SkipIfNotReady() {
        if (!s_hwnd)
            GTEST_SKIP() << (s_skip_reason.empty()
                ? "the WinUI3 sample app is not available"
                : s_skip_reason);
        // Every test in this fixture shares one app instance, so if it has gone
        // away the remaining tests would all fail with unrelated-looking
        // errors. Saying so once is far easier to act on.
        if (!IsWindow(s_hwnd))
            GTEST_SKIP() << "the WinUI3 sample app's window closed during the run";
    }

    static std::string hwnd_string() {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%llX",
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s_hwnd)));
        return buf;
    }

    static DWORD process_id() {
        return s_process ? GetProcessId(s_process.get()) : 0;
    }

    static std::string secondary_hwnd_string() {
        struct Search {
            DWORD pid;
            HWND hwnd = nullptr;
        } search{GetProcessId(s_process.get())};
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* state = reinterpret_cast<Search*>(lParam);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != state->pid || !IsWindowVisible(hwnd))
                return TRUE;
            char title[256]{};
            GetWindowTextA(hwnd, title, sizeof(title));
            if (strcmp(title, "LVT Native Secondary") != 0)
                return TRUE;
            state->hwnd = hwnd;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&search));
        if (!search.hwnd)
            return {};
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "0x%p",
                 static_cast<void*>(search.hwnd));
        return buffer;
    }

    // Connect and return the session id, failing the test if it does not work.
    static std::string connect(McpClient& client, const std::string& mode = "uia") {
        auto result = client.call_tool(
            "connect", json{{"hwnd", hwnd_string()}, {"mode", mode}});
        return result.value("session", "");
    }

    // The sample app's click counter, used to prove an action actually landed
    // rather than merely reporting success.
    static std::string status_text(McpClient& client, const std::string& session) {
        auto found = client.call_tool(
            "find_elements", json{{"session", session}, {"automationId", "StatusText"}});
        return found["elements"].empty() ? std::string() : found["elements"][0].value("text", "");
    }

    static wil::unique_process_handle s_process;
    static wil::unique_handle s_thread;
    static HWND s_hwnd;
    static std::string s_skip_reason;
};

wil::unique_process_handle McpSampleFixture::s_process;
wil::unique_handle McpSampleFixture::s_thread;
HWND McpSampleFixture::s_hwnd = nullptr;
std::string McpSampleFixture::s_skip_reason;

class NativeMcpFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        const fs::path exe = NATIVE_CONTROLS_FIXTURE_EXE_PATH;
        ASSERT_TRUE(fs::exists(exe));

        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        ASSERT_TRUE(CreateProcessW(
            nullptr, command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup, &process));
        s_process.reset(process.hProcess);
        s_thread.reset(process.hThread);
        s_pid = process.dwProcessId;
        WaitForInputIdle(s_process.get(), 5000);

        for (int attempt = 0; attempt < 50 && !s_hwnd; ++attempt) {
            EnumWindows(
                [](HWND hwnd, LPARAM parameter) -> BOOL {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(hwnd, &pid);
                    if (pid != s_pid)
                        return TRUE;
                    wchar_t className[128]{};
                    GetClassNameW(
                        hwnd, className,
                        static_cast<int>(_countof(className)));
                    if (wcscmp(
                            className,
                            native_fixture::kWindowClass) == 0) {
                        *reinterpret_cast<HWND*>(parameter) = hwnd;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&s_hwnd));
            if (!s_hwnd)
                Sleep(100);
        }
        ASSERT_NE(s_hwnd, nullptr);
    }

    static void TearDownTestSuite() {
        if (s_hwnd && IsWindow(s_hwnd))
            PostMessageW(s_hwnd, native_fixture::kCloseMessage, 0, 0);
        if (s_process)
            WaitForSingleObject(s_process.get(), 5000);
        s_process.reset();
        s_thread.reset();
        s_hwnd = nullptr;
        s_pid = 0;
    }

    static std::string hwnd_string() {
        char value[32]{};
        snprintf(
            value, sizeof(value), "0x%llX",
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(s_hwnd)));
        return value;
    }

    static std::string connect(McpClient& client) {
        auto connected = client.call_tool(
            "connect",
            json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
        return connected.value("session", "");
    }

    static std::string control_key(int id) {
        const HWND hwnd = GetDlgItem(s_hwnd, id);
        if (!hwnd)
            return {};
        char value[64]{};
        snprintf(
            value, sizeof(value), "win32:0x%llX",
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(hwnd)));
        return value;
    }

    static const json* find_by_class(
        const json& tree, const std::string& className) {
        std::vector<const json*> elements;
        collect_json_elements(tree, elements);
        for (const auto* element : elements) {
            if (element->value("className", "") == className)
                return element;
        }
        return nullptr;
    }

    static const json* find_child(
        const json& parent, const std::string& type,
        const std::string& text) {
        std::vector<const json*> elements;
        collect_json_elements(parent, elements);
        for (const auto* element : elements) {
            if (element->value("type", "") == type &&
                element->value("text", "") == text) {
                return element;
            }
        }
        return nullptr;
    }

    static wil::unique_process_handle s_process;
    static wil::unique_handle s_thread;
    static DWORD s_pid;
    static HWND s_hwnd;
};

wil::unique_process_handle NativeMcpFixture::s_process;
wil::unique_handle NativeMcpFixture::s_thread;
DWORD NativeMcpFixture::s_pid = 0;
HWND NativeMcpFixture::s_hwnd = nullptr;

} // namespace

// --- handshake and tool listing ----------------------------------------

TEST(McpServer, CompletesTheHandshakeAndIdentifiesItself) {
    McpClient client(false);
    ASSERT_TRUE(client.started()) << "could not start `lvt mcp`";

    json info;
    ASSERT_TRUE(client.handshake(&info)) << "the server did not answer initialize";

    EXPECT_EQ(info["serverInfo"].value("name", ""), "lvt");
    // The version comes from lvt's own build, so a wrong one means the C ABI is
    // reporting something other than the binary the server is linked into.
    EXPECT_FALSE(info["serverInfo"].value("version", "").empty());
    EXPECT_FALSE(info.value("protocolVersion", "").empty());
    EXPECT_TRUE(info["capabilities"].contains("tools"));
    EXPECT_FALSE(info.value("instructions", "").empty())
        << "instructions are how a model learns the connect-first workflow";
}

TEST(McpServer, ReadOnlyByDefaultAndInputOnlyWithTheFlag) {
    // The security property of the whole server: without --allow-input, no tool
    // that can change the target application is even advertised, so a model
    // cannot be talked into trying one.
    static constexpr const char* kMutating[] = {
        "click", "type_text", "press_key", "set_value", "toggle", "invoke",
        "scroll", "select", "set_expanded", "focus", "select_text", "window_action",
        "set_property", "clear_property",
    };

    {
        McpClient readOnly(false);
        ASSERT_TRUE(readOnly.started());
        ASSERT_TRUE(readOnly.handshake());
        const auto names = tool_names(readOnly.request("tools/list")["result"]);
        ASSERT_FALSE(names.empty());
        for (const char* tool : kMutating)
            EXPECT_FALSE(has_tool(names, tool)) << tool << " must not be exposed by default";
        // Inspection must still be there, or the read-only mode is useless.
        EXPECT_TRUE(has_tool(names, "get_uia_tree"));
        EXPECT_TRUE(has_tool(names, "connect"));
        EXPECT_TRUE(has_tool(names, "screenshot"));
    }

    {
        McpClient full(true);
        ASSERT_TRUE(full.started());
        ASSERT_TRUE(full.handshake());
        const auto names = tool_names(full.request("tools/list")["result"]);
        for (const char* tool : kMutating)
            EXPECT_TRUE(has_tool(names, tool)) << tool << " should be exposed with --allow-input";
    }
}

#if LVT_ENABLE_WPF && LVT_WITH_MANAGED
TEST(McpManagedFrameworks, WpfConnectionPersistsAcrossTreeReads) {
    verify_managed_mcp_connection(
        WPF_SAMPLE_EXE_PATH, "OkButton", "wpf:0x", L"lvt_wpf_tap.log");
}

TEST(McpManagedFrameworks, WpfTypedDependencyProperties) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(WPF_SAMPLE_EXE_PATH));
    const int startsBefore =
        count_managed_tap_starts(L"lvt_wpf_tap.log", sample.pid());

    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    auto connected = client.call_tool(
        "connect", json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    const std::string session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);

    auto tree = client.call_tool(
        "get_visual_tree", json{{"session", session}});
    ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
    const json* button = find_managed_named_element(tree["root"], "OkButton");
    const json* textBox = find_managed_named_element(tree["root"], "NameBox");
    const json* window =
        find_managed_named_element(tree["root"], "MainWindowRoot");
    ASSERT_NE(button, nullptr);
    ASSERT_NE(textBox, nullptr);
    ASSERT_NE(window, nullptr);
    const std::string buttonKey = button->value("key", "");
    const std::string textBoxKey = textBox->value("key", "");
    ASSERT_EQ(buttonKey.rfind("wpf:0x", 0), 0u);
    ASSERT_EQ(textBoxKey.rfind("wpf:0x", 0), 0u);

    bool isError = false;
    auto buttonProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", buttonKey}}, &isError);
    ASSERT_FALSE(isError) << buttonProperties.dump(2);
    auto repeatedButtonProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", buttonKey}}, &isError);
    ASSERT_FALSE(isError) << repeatedButtonProperties.dump(2);
    EXPECT_EQ(
        repeatedButtonProperties.value("schemaId", ""),
        buttonProperties.value("schemaId", ""));

    const auto* opacity = find_property_descriptor(buttonProperties, "Opacity");
    const auto* enabled = find_property_descriptor(buttonProperties, "IsEnabled");
    const auto* alignment =
        find_property_descriptor(buttonProperties, "HorizontalAlignment");
    ASSERT_NE(opacity, nullptr) << buttonProperties.dump(2);
    ASSERT_NE(enabled, nullptr) << buttonProperties.dump(2);
    ASSERT_NE(alignment, nullptr) << buttonProperties.dump(2);
    EXPECT_EQ(opacity->value("kind", ""), "number");
    EXPECT_EQ(enabled->value("kind", ""), "boolean");
    EXPECT_EQ(alignment->value("kind", ""), "enum");
    EXPECT_TRUE(opacity->value("supportsClear", false));
    EXPECT_TRUE(enabled->value("supportsClear", false));
    EXPECT_TRUE(alignment->value("supportsClear", false));
    EXPECT_EQ(find_property_descriptor(buttonProperties, "ActualWidth"), nullptr)
        << "read-only dependency properties must be excluded";

    auto windowProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", window->value("key", "")}},
        &isError);
    ASSERT_FALSE(isError) << windowProperties.dump(2);
    EXPECT_EQ(
        find_property_descriptor(windowProperties, "OrdinaryClrProperty"),
        nullptr) << "ordinary CLR properties must not enter the WPF DP schema";

    std::set<std::string> alignmentChoices;
    for (const auto& choice : alignment->value("choices", json::array()))
        alignmentChoices.insert(choice.value("value", ""));
    EXPECT_TRUE(alignmentChoices.contains("Left"));
    EXPECT_TRUE(alignmentChoices.contains("Right"));
    EXPECT_TRUE(alignmentChoices.contains("Stretch"));

    const std::string opacityId = opacity->value("descriptorId", "");
    const std::string enabledId = enabled->value("descriptorId", "");
    const std::string alignmentId = alignment->value("descriptorId", "");
    ASSERT_FALSE(opacityId.empty());
    ASSERT_FALSE(enabledId.empty());
    ASSERT_FALSE(alignmentId.empty());
    EXPECT_NEAR(
        std::stod(typed_property_value(buttonProperties, "Opacity")), 0.75, 0.001);
    EXPECT_EQ(typed_property_value(buttonProperties, "IsEnabled"), "true");
    EXPECT_EQ(
        typed_property_value(buttonProperties, "HorizontalAlignment"), "Left");

    auto setOpacity = client.call_tool(
        "set_property",
        json{{"session", session}, {"element", buttonKey},
             {"descriptorId", opacityId}, {"value", "0.4"}},
        &isError);
    ASSERT_FALSE(isError) << setOpacity.dump(2);
    EXPECT_NEAR(std::stod(setOpacity.value("value", "")), 0.4, 0.001);
    auto changedButton = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", buttonKey}}, &isError);
    ASSERT_FALSE(isError) << changedButton.dump(2);
    const auto* changedOpacityValue =
        find_property_value(changedButton, opacityId);
    ASSERT_NE(changedOpacityValue, nullptr);
    EXPECT_TRUE(changedOpacityValue->value("canClear", false));
    EXPECT_TRUE(changedOpacityValue->value("overridden", false));
    EXPECT_EQ(changedOpacityValue->value("source", ""), "Local");
    auto clearOpacity = client.call_tool(
        "clear_property",
        json{{"session", session}, {"element", buttonKey},
             {"descriptorId", opacityId}},
        &isError);
    ASSERT_FALSE(isError) << clearOpacity.dump(2);
    EXPECT_TRUE(clearOpacity.value("cleared", false));
    EXPECT_NEAR(std::stod(clearOpacity.value("value", "")), 0.75, 0.001);
    auto restoredButton = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", buttonKey}}, &isError);
    ASSERT_FALSE(isError) << restoredButton.dump(2);
    const auto* restoredOpacity =
        find_property_value(restoredButton, opacityId);
    ASSERT_NE(restoredOpacity, nullptr);
    EXPECT_FALSE(restoredOpacity->value("canClear", true));
    EXPECT_FALSE(restoredOpacity->value("overridden", true));
    EXPECT_NE(restoredOpacity->value("source", ""), "Local");

    auto setEnabled = client.call_tool(
        "set_property",
        json{{"session", session}, {"element", buttonKey},
             {"descriptorId", enabledId}, {"value", "false"}},
        &isError);
    ASSERT_FALSE(isError) << setEnabled.dump(2);
    EXPECT_EQ(setEnabled.value("value", ""), "false");
    auto clearEnabled = client.call_tool(
        "clear_property",
        json{{"session", session}, {"element", buttonKey},
             {"descriptorId", enabledId}},
        &isError);
    ASSERT_FALSE(isError) << clearEnabled.dump(2);
    EXPECT_EQ(clearEnabled.value("value", ""), "true");

    auto setAlignment = client.call_tool(
        "set_property",
        json{{"session", session}, {"element", buttonKey},
             {"descriptorId", alignmentId}, {"value", "Right"}},
        &isError);
    ASSERT_FALSE(isError) << setAlignment.dump(2);
    EXPECT_EQ(setAlignment.value("value", ""), "Right");
    auto clearAlignment = client.call_tool(
        "clear_property",
        json{{"session", session}, {"element", buttonKey},
             {"descriptorId", alignmentId}},
        &isError);
    ASSERT_FALSE(isError) << clearAlignment.dump(2);
    EXPECT_EQ(clearAlignment.value("value", ""), "Left");

    auto textProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", textBoxKey}}, &isError);
    ASSERT_FALSE(isError) << textProperties.dump(2);
    const auto* text = find_property_descriptor(textProperties, "Text");
    ASSERT_NE(text, nullptr) << textProperties.dump(2);
    EXPECT_EQ(text->value("kind", ""), "string");
    const std::string textId = text->value("descriptorId", "");
    auto setText = client.call_tool(
        "set_property",
        json{{"session", session}, {"element", textBoxKey},
             {"descriptorId", textId}, {"value", "Grace Hopper"}},
        &isError);
    ASSERT_FALSE(isError) << setText.dump(2);
    EXPECT_EQ(setText.value("value", ""), "Grace Hopper");
    auto clearText = client.call_tool(
        "clear_property",
        json{{"session", session}, {"element", textBoxKey},
             {"descriptorId", textId}},
        &isError);
    ASSERT_FALSE(isError) << clearText.dump(2);
    EXPECT_EQ(clearText.value("value", ""), "");

    auto deadIdentity = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", "wpf:0x7FFFFFFFFFFFFFFF"}},
        &isError);
    EXPECT_TRUE(isError) << deadIdentity.dump(2);
    EXPECT_NE(deadIdentity.value("error", "").find("dead"), std::string::npos);

    auto disconnected = client.call_tool(
        "disconnect", json{{"session", session}}, &isError);
    ASSERT_FALSE(isError) << disconnected.dump(2);
    EXPECT_EQ(
        count_managed_tap_starts(L"lvt_wpf_tap.log", sample.pid()) -
            startsBefore,
        1) << "tree and dependency-property commands must share one injection";

    connected = client.call_tool(
        "connect", json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    const std::string reconnectedSession = connected.value("session", "");
    ASSERT_FALSE(reconnectedSession.empty()) << connected.dump(2);
    auto hydratedProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", reconnectedSession}, {"element", buttonKey}},
        &isError);
    ASSERT_FALSE(isError) << hydratedProperties.dump(2);
    EXPECT_NE(
        hydratedProperties.value("schemaId", ""),
        buttonProperties.value("schemaId", ""));
    auto staleDescriptor = client.call_tool(
        "set_property",
        json{{"session", reconnectedSession},
             {"element", buttonKey},
             {"descriptorId", opacityId},
             {"value", "0.5"}},
        &isError);
    EXPECT_TRUE(isError) << staleDescriptor.dump(2);
    EXPECT_NE(
        staleDescriptor.value("error", "").find("stale"),
        std::string::npos);
    client.call_tool(
        "disconnect", json{{"session", reconnectedSession}}, &isError);
    ASSERT_FALSE(isError);
}
#endif

#if LVT_ENABLE_WINFORMS && LVT_WITH_MANAGED
TEST(McpManagedFrameworks, WinFormsConnectionPersistsAcrossTreeReads) {
    verify_managed_mcp_connection(
        WINFORMS_SAMPLE_EXE_PATH, "okButton", "winforms:0x",
        L"lvt_winforms_tap.log");
}

TEST(McpManagedFrameworks, WinFormsTypedPropertiesAreConservative) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(WINFORMS_SAMPLE_EXE_PATH));
    const int startsBefore =
        count_managed_tap_starts(L"lvt_winforms_tap.log", sample.pid());

    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    auto connected = client.call_tool(
        "connect", json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    const std::string session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);
    auto tree = client.call_tool(
        "get_visual_tree", json{{"session", session}});
    ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
    const json* form = find_managed_named_element(tree["root"], "MainForm");
    ASSERT_NE(form, nullptr);
    const std::string formKey = form->value("key", "");
    ASSERT_EQ(formKey.rfind("winforms:0x", 0), 0u);

    bool isError = false;
    auto properties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", formKey}}, &isError);
    ASSERT_FALSE(isError) << properties.dump(2);
    auto repeated = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", formKey}}, &isError);
    ASSERT_FALSE(isError) << repeated.dump(2);
    EXPECT_EQ(
        repeated.value("schemaId", ""), properties.value("schemaId", ""));

    const auto* text = find_property_descriptor(properties, "EditableText");
    const auto* retry = find_property_descriptor(properties, "RetryCount");
    const auto* mode = find_property_descriptor(properties, "Mode");
    const auto* providerText =
        find_property_descriptor(properties, "ProviderText");
    const auto* throwing =
        find_property_descriptor(properties, "ThrowingValue");
    ASSERT_NE(text, nullptr) << properties.dump(2);
    ASSERT_NE(retry, nullptr) << properties.dump(2);
    ASSERT_NE(mode, nullptr) << properties.dump(2);
    ASSERT_NE(providerText, nullptr) << properties.dump(2);
    ASSERT_NE(throwing, nullptr) << properties.dump(2);
    EXPECT_EQ(text->value("kind", ""), "string");
    EXPECT_EQ(retry->value("kind", ""), "integer");
    EXPECT_EQ(mode->value("kind", ""), "enum");
    EXPECT_EQ(providerText->value("kind", ""), "string");
    EXPECT_TRUE(text->value("supportsClear", false));
    EXPECT_TRUE(providerText->value("supportsClear", false));
    EXPECT_EQ(find_property_descriptor(properties, "ReadOnlyValue"), nullptr);
    EXPECT_EQ(find_property_descriptor(properties, "UnsafeFontProperty"), nullptr);
    EXPECT_EQ(find_property_descriptor(properties, "Controls"), nullptr);

    std::set<std::string> modeChoices;
    for (const auto& choice : mode->value("choices", json::array()))
        modeChoices.insert(choice.value("value", ""));
    EXPECT_EQ(
        modeChoices,
        (std::set<std::string>{"Advanced", "Basic", "Expert"}));

    const std::string textId = text->value("descriptorId", "");
    const std::string retryId = retry->value("descriptorId", "");
    const std::string modeId = mode->value("descriptorId", "");
    const std::string providerTextId =
        providerText->value("descriptorId", "");
    const std::string throwingId = throwing->value("descriptorId", "");

    auto setText = client.call_tool(
        "set_property",
        json{{"session", session}, {"element", formKey},
             {"descriptorId", textId}, {"value", "Changed text"}},
        &isError);
    ASSERT_FALSE(isError) << setText.dump(2);
    EXPECT_EQ(setText.value("value", ""), "Changed text");
    auto changed = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", formKey}}, &isError);
    ASSERT_FALSE(isError) << changed.dump(2);
    const auto* changedText = find_property_value(changed, textId);
    ASSERT_NE(changedText, nullptr);
    EXPECT_TRUE(changedText->value("canClear", false));
    EXPECT_TRUE(changedText->value("overridden", false));
    EXPECT_EQ(changedText->value("source", ""), "Modified");
    auto clearText = client.call_tool(
        "clear_property",
        json{{"session", session}, {"element", formKey},
             {"descriptorId", textId}},
        &isError);
    ASSERT_FALSE(isError) << clearText.dump(2);
    EXPECT_TRUE(clearText.value("cleared", false));
    EXPECT_EQ(clearText.value("value", ""), "Default text");
    auto resetSnapshot = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", formKey}}, &isError);
    ASSERT_FALSE(isError) << resetSnapshot.dump(2);
    const auto* resetTextValue =
        find_property_value(resetSnapshot, textId);
    ASSERT_NE(resetTextValue, nullptr);
    EXPECT_FALSE(resetTextValue->value("canClear", true));
    EXPECT_FALSE(resetTextValue->value("overridden", true));
    EXPECT_EQ(resetTextValue->value("source", ""), "Default");

    auto setRetry = client.call_tool(
        "set_property",
        json{{"session", session}, {"element", formKey},
             {"descriptorId", retryId}, {"value", "9"}},
        &isError);
    ASSERT_FALSE(isError) << setRetry.dump(2);
    EXPECT_EQ(setRetry.value("value", ""), "9");
    auto clearRetry = client.call_tool(
        "clear_property",
        json{{"session", session}, {"element", formKey},
             {"descriptorId", retryId}},
        &isError);
    ASSERT_FALSE(isError) << clearRetry.dump(2);
    EXPECT_EQ(clearRetry.value("value", ""), "5");

    auto setMode = client.call_tool(
        "set_property",
        json{{"session", session}, {"element", formKey},
             {"descriptorId", modeId}, {"value", "Advanced"}},
        &isError);
    ASSERT_FALSE(isError) << setMode.dump(2);
    EXPECT_EQ(setMode.value("value", ""), "Advanced");
    auto clearMode = client.call_tool(
        "clear_property",
        json{{"session", session}, {"element", formKey},
             {"descriptorId", modeId}},
        &isError);
    ASSERT_FALSE(isError) << clearMode.dump(2);
    EXPECT_EQ(clearMode.value("value", ""), "Basic");

    auto setProviderText = client.call_tool(
        "set_property",
        json{{"session", session}, {"element", formKey},
             {"descriptorId", providerTextId}, {"value", "Custom descriptor"}},
        &isError);
    ASSERT_FALSE(isError) << setProviderText.dump(2);
    EXPECT_EQ(setProviderText.value("value", ""), "Custom descriptor");
    auto clearProviderText = client.call_tool(
        "clear_property",
        json{{"session", session}, {"element", formKey},
             {"descriptorId", providerTextId}},
        &isError);
    ASSERT_FALSE(isError) << clearProviderText.dump(2);
    EXPECT_EQ(clearProviderText.value("value", ""), "Provider default");

    auto setterFailure = client.call_tool(
        "set_property",
        json{{"session", session}, {"element", formKey},
             {"descriptorId", throwingId}, {"value", "rejected"}},
        &isError);
    EXPECT_TRUE(isError) << setterFailure.dump(2);
    EXPECT_NE(
        setterFailure.value("error", "").find("rejected"),
        std::string::npos);

    auto invalidNumber = client.call_tool(
        "set_property",
        json{{"session", session}, {"element", formKey},
             {"descriptorId", retryId}, {"value", "9.5"}},
        &isError);
    EXPECT_TRUE(isError) << invalidNumber.dump(2);

    auto deadIdentity = client.call_tool(
        "get_editable_properties",
        json{{"session", session},
             {"element", "winforms:0x7FFFFFFFFFFFFFFF"}},
        &isError);
    EXPECT_TRUE(isError) << deadIdentity.dump(2);
    EXPECT_NE(deadIdentity.value("error", "").find("dead"), std::string::npos);

    auto disconnected = client.call_tool(
        "disconnect", json{{"session", session}}, &isError);
    ASSERT_FALSE(isError) << disconnected.dump(2);
    EXPECT_EQ(
        count_managed_tap_starts(L"lvt_winforms_tap.log", sample.pid()) -
            startsBefore,
        1) << "tree and TypeDescriptor property commands must share one injection";

    connected = client.call_tool(
        "connect", json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    const std::string reconnectedSession = connected.value("session", "");
    ASSERT_FALSE(reconnectedSession.empty()) << connected.dump(2);
    auto hydratedProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", reconnectedSession}, {"element", formKey}},
        &isError);
    ASSERT_FALSE(isError) << hydratedProperties.dump(2);
    EXPECT_NE(
        hydratedProperties.value("schemaId", ""),
        properties.value("schemaId", ""));
    auto staleDescriptor = client.call_tool(
        "set_property",
        json{{"session", reconnectedSession},
             {"element", formKey},
             {"descriptorId", textId},
             {"value", "stale"}},
        &isError);
    EXPECT_TRUE(isError) << staleDescriptor.dump(2);
    EXPECT_NE(
        staleDescriptor.value("error", "").find("stale"),
        std::string::npos);
    client.call_tool(
        "disconnect", json{{"session", reconnectedSession}}, &isError);
    ASSERT_FALSE(isError);
}

TEST(McpManagedFrameworks, FailedWinFormsRefreshDoesNotAdvanceSnapshot) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(WINFORMS_SAMPLE_EXE_PATH));

    McpClient owner(false);
    ASSERT_TRUE(owner.started());
    ASSERT_TRUE(owner.handshake());
    auto ownerConnected = owner.call_tool(
        "connect", json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    const std::string ownerSession = ownerConnected.value("session", "");
    ASSERT_FALSE(ownerSession.empty()) << ownerConnected.dump(2);
    auto ownerTree = owner.call_tool(
        "get_visual_tree", json{{"session", ownerSession}});
    ASSERT_TRUE(ownerTree.contains("root")) << ownerTree.dump(2);

    McpClient observer(false);
    ASSERT_TRUE(observer.started());
    ASSERT_TRUE(observer.handshake());
    auto observerConnected = observer.call_tool(
        "connect", json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    const std::string observerSession =
        observerConnected.value("session", "");
    ASSERT_FALSE(observerSession.empty()) << observerConnected.dump(2);

    bool isError = false;
    auto failed = observer.call_tool(
        "get_visual_tree_changes",
        json{{"session", observerSession}}, &isError);
    EXPECT_TRUE(isError) << failed.dump(2);
    EXPECT_NE(
        failed.value("error", "").find("temporarily unavailable"),
        std::string::npos) << failed.dump(2);

    owner.call_tool(
        "disconnect", json{{"session", ownerSession}}, &isError);
    ASSERT_FALSE(isError);

    auto recovered = observer.call_tool(
        "get_visual_tree_changes",
        json{{"session", observerSession}}, &isError);
    ASSERT_FALSE(isError) << recovered.dump(2);
    EXPECT_TRUE(recovered.value("snapshot", false))
        << "the failed host-only refresh must not create or advance a baseline";
    EXPECT_FALSE(recovered.value("events", json::array()).empty());
    observer.call_tool(
        "disconnect", json{{"session", observerSession}}, &isError);
    ASSERT_FALSE(isError);
}

TEST(McpManagedFrameworks, DisconnectWinsQueuedPropertyRequests) {
    ManagedSampleProcess sample;
    ASSERT_TRUE(sample.start(WINFORMS_SAMPLE_EXE_PATH));

    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    auto connected = client.call_tool(
        "connect", json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    std::string session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);
    auto tree = client.call_tool(
        "get_visual_tree", json{{"session", session}});
    const json* form = find_managed_named_element(tree["root"], "MainForm");
    ASSERT_NE(form, nullptr);
    const std::string formKey = form->value("key", "");
    bool isError = false;
    auto properties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", formKey}}, &isError);
    ASSERT_FALSE(isError) << properties.dump(2);
    const auto* text = find_property_descriptor(properties, "EditableText");
    ASSERT_NE(text, nullptr);
    const std::string textId = text->value("descriptorId", "");

    ManagedSampleUiBlock block;
    ASSERT_TRUE(block.enter(L"WinForms", sample.pid()));
    const int slowTreeId = client.send_request(
        "tools/call",
        json{{"name", "get_visual_tree"},
             {"arguments", json{{"session", session}}}});
    Sleep(150);
    const int setId = client.send_request(
        "tools/call",
        json{{"name", "set_property"},
             {"arguments",
              json{{"session", session},
                   {"element", formKey},
                   {"descriptorId", textId},
                   {"value", "must not land"}}}});
    const int disconnectId = client.send_request(
        "tools/call",
        json{{"name", "disconnect"},
             {"arguments", json{{"session", session}}}});
    Sleep(150);
    block.release();

    auto slowTreeResponse = client.await_response(slowTreeId);
    auto setResponse = client.await_response(setId);
    auto disconnectResponse = client.await_response(disconnectId);
    ASSERT_TRUE(slowTreeResponse.contains("result")) << slowTreeResponse.dump(2);
    ASSERT_TRUE(setResponse.contains("result")) << setResponse.dump(2);
    EXPECT_TRUE(setResponse["result"].value("isError", false))
        << "a property mutation queued behind disconnect must be refused";
    ASSERT_TRUE(disconnectResponse.contains("result"))
        << disconnectResponse.dump(2);
    EXPECT_FALSE(disconnectResponse["result"].value("isError", false));

    connected = client.call_tool(
        "connect", json{{"hwnd", sample.hwnd_string()}, {"mode", "visual"}});
    session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);
    properties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", formKey}}, &isError);
    ASSERT_FALSE(isError) << properties.dump(2);
    EXPECT_EQ(typed_property_value(properties, "EditableText"), "Default text")
        << "disconnect must prevent the queued mutation and connection-map leak";
    text = find_property_descriptor(properties, "EditableText");
    ASSERT_NE(text, nullptr);

    ASSERT_TRUE(block.enter(L"WinForms", sample.pid()));
    const int secondSlowTreeId = client.send_request(
        "tools/call",
        json{{"name", "get_visual_tree"},
             {"arguments", json{{"session", session}}}});
    Sleep(150);
    const int getId = client.send_request(
        "tools/call",
        json{{"name", "get_editable_properties"},
             {"arguments",
              json{{"session", session}, {"element", formKey}}}});
    const int secondDisconnectId = client.send_request(
        "tools/call",
        json{{"name", "disconnect"},
             {"arguments", json{{"session", session}}}});
    Sleep(150);
    block.release();

    (void)client.await_response(secondSlowTreeId);
    auto getResponse = client.await_response(getId);
    auto secondDisconnectResponse =
        client.await_response(secondDisconnectId);
    ASSERT_TRUE(getResponse.contains("result")) << getResponse.dump(2);
    EXPECT_TRUE(getResponse["result"].value("isError", false))
        << "a property read queued behind disconnect must be refused";
    ASSERT_TRUE(secondDisconnectResponse.contains("result"))
        << secondDisconnectResponse.dump(2);
    EXPECT_FALSE(secondDisconnectResponse["result"].value("isError", false));
}
#endif

TEST(McpServer, CallingAWithheldToolIsRejectedRatherThanSilentlyIgnored) {
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    // Not merely absent from tools/list: actually calling it must fail, or the
    // gate would be advisory rather than enforced.
    auto response = client.request("tools/call",
                                   json{{"name", "click"},
                                        {"arguments", json{{"session", "s1"}, {"element", "e0"}}}});
    ASSERT_FALSE(response.is_null());
    EXPECT_TRUE(response.contains("error"))
        << "a withheld tool must be refused, got: " << response.dump(2);
}

TEST(McpServer, EveryToolAdvertisesADescriptionAndSchema) {
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    auto tools = client.request("tools/list")["result"]["tools"];
    ASSERT_FALSE(tools.empty());
    for (const auto& tool : tools) {
        const auto name = tool.value("name", "");
        EXPECT_FALSE(name.empty());
        // A model picks tools by description alone, so an empty or throwaway
        // one is a functional defect rather than a documentation nit.
        EXPECT_GT(tool.value("description", "").size(), 30u)
            << name << " needs a description explaining when to use it";
        ASSERT_TRUE(tool.contains("inputSchema")) << name;
        EXPECT_EQ(tool["inputSchema"].value("type", ""), "object") << name;
    }
}

// --- error handling -----------------------------------------------------

TEST(McpServer, ToolFailuresComeBackAsToolErrorsNotProtocolErrors) {
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    // "no such session" is information the model should act on, so it must
    // arrive as a tool result it can read, not as a transport failure.
    bool isError = false;
    auto result = client.call_tool("get_uia_tree", json{{"session", "s999"}}, &isError);
    EXPECT_TRUE(isError);
    EXPECT_NE(result.value("error", "").find("unknown session"), std::string::npos)
        << result.dump(2);
    EXPECT_FALSE(result.value("ok", true));
}

TEST(McpServer, TypedPropertyMissingSessionsHaveDisposition) {
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    const std::array<std::pair<const char*, json>, 3> calls{{
        {"get_editable_properties",
         json{{"session", "missing-session"}, {"element", "uia:1"}}},
        {"set_property",
         json{{"session", "missing-session"},
              {"element", "uia:1"},
              {"descriptorId", "missing"},
              {"value", "value"}}},
        {"clear_property",
         json{{"session", "missing-session"},
              {"element", "uia:1"},
              {"descriptorId", "missing"}}},
    }};
    for (const auto& [tool, arguments] : calls) {
        bool isError = false;
        auto result = client.call_tool(tool, arguments, &isError);
        EXPECT_TRUE(isError) << tool << ": " << result.dump(2);
        expect_typed_property_session_disconnected(result);
    }
}

TEST(McpServer, ConnectingToNothingFailsWithAReadableMessage) {
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    bool isError = false;
    auto result = client.call_tool(
        "connect", json{{"name", "no_such_process_exists_xyzzy"}}, &isError);
    EXPECT_TRUE(isError);
    EXPECT_FALSE(result.value("error", "").empty()) << result.dump(2);
}

TEST(McpServer, SurvivesAMalformedRequestAndKeepsServing) {
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    // An unknown method must not take the server down: a host that sends one
    // should still be able to carry on.
    client.request("no/such/method");
    auto tools = client.request("tools/list");
    ASSERT_FALSE(tools.is_null()) << "the server stopped answering after an unknown method";
    EXPECT_FALSE(tool_names(tools["result"]).empty());
}

TEST(McpServer, ExitsCleanlyWhenTheClientClosesTheStream) {
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    client.shutdown();
    // A host disconnecting is the normal end of a session, so it is a clean
    // exit rather than a failure — hosts surface a non-zero code as a crash.
    EXPECT_EQ(client.exit_code(), 0u);
}

// --- inspection against a real app -------------------------------------

TEST_F(McpSampleFixture, ConnectReportsTheTargetAndOpensASession) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    auto result = client.call_tool("connect", json{{"hwnd", hwnd_string()}});
    EXPECT_FALSE(result.value("session", "").empty()) << result.dump(2);
    EXPECT_GT(result.value("pid", 0u), 0u);
    EXPECT_FALSE(result.value("architecture", "").empty());
    EXPECT_FALSE(result["frameworks"].empty()) << "the sample app is WinUI 3";
}

TEST_F(McpSampleFixture, SessionsAreIndependentAndDisconnectReleasesThem) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    const auto first = connect(client);
    const auto second = connect(client);
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    EXPECT_NE(first, second) << "each connect must yield its own session";

    client.call_tool("disconnect", json{{"session", first}});

    // Closing one session must not disturb the other: the whole point of the
    // multi-target design is that an agent can hold several apps at once.
    bool isError = false;
    client.call_tool("get_uia_tree", json{{"session", second}, {"depth", 1}}, &isError);
    EXPECT_FALSE(isError) << "the surviving session stopped working";

    client.call_tool("get_uia_tree", json{{"session", first}, {"depth", 1}}, &isError);
    EXPECT_TRUE(isError) << "a disconnected session must stop resolving";
}

TEST_F(McpSampleFixture, UiaTreeCarriesAutomationIdsAndRespectsDepth) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto shallow = client.call_tool("get_uia_tree", json{{"session", session}, {"depth", 0}});
    ASSERT_TRUE(shallow.contains("root")) << shallow.dump(2);
    EXPECT_TRUE(shallow["root"].value("children", json::array()).empty())
        << "depth 0 should return the root alone";

    auto full = client.call_tool("get_uia_tree", json{{"session", session}});
    ASSERT_TRUE(full.contains("root"));
    EXPECT_FALSE(full["root"]["children"].empty());

    // The AutomationIds are what make the tree usable for automation at all.
    const auto text = full.dump();
    EXPECT_NE(text.find("PrimaryButton"), std::string::npos);
    EXPECT_NE(text.find("AutomationId"), std::string::npos);
}

TEST_F(McpSampleFixture, FindElementsMatchesByAutomationIdTypeAndPattern) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto byId = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    ASSERT_EQ(byId["elements"].size(), 1u) << byId.dump(2);
    EXPECT_EQ(byId["elements"][0].value("type", ""), "Button");

    auto byType = client.call_tool(
        "find_elements", json{{"session", session}, {"type", "Button"}});
    EXPECT_GE(byType["elements"].size(), 1u);

    auto byPattern = client.call_tool(
        "find_elements", json{{"session", session}, {"pattern", "Invoke"}});
    EXPECT_GE(byPattern["elements"].size(), 1u)
        << "the sample app has invokable buttons: " << byPattern.dump(2);

    // A limit must actually bound the result, or a large app floods the model's
    // context with a single call.
    auto limited = client.call_tool(
        "find_elements", json{{"session", session}, {"type", ""}, {"limit", 3}});
    EXPECT_LE(limited["elements"].size(), 3u);

    auto none = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "NoSuchControl"}});
    EXPECT_TRUE(none["elements"].empty());
}

TEST_F(McpSampleFixture, HitTestFindsTheSmallestElementAtAPointWithItsAncestors) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    ASSERT_EQ(found["elements"].size(), 1u);
    const auto& button = found["elements"][0];
    const auto& bounds = button["bounds"];

    auto hit = client.call_tool("hit_test",
                                json{{"session", session},
                                     {"x", bounds["x"].get<int>() + bounds["width"].get<int>() / 2},
                                     {"y", bounds["y"].get<int>() + bounds["height"].get<int>() / 2}});
    ASSERT_TRUE(hit.contains("element")) << hit.dump(2);

    // Either the button itself or something inside it; in both cases the button
    // has to appear in the chain, or hit-testing is pointing somewhere else.
    const auto hitId = hit["element"].value("id", "");
    const auto buttonId = button.value("id", "");
    bool inChain = hitId == buttonId;
    for (const auto& ancestor : hit["ancestors"])
        inChain = inChain || ancestor.get<std::string>() == buttonId;
    EXPECT_TRUE(inChain) << "hit-testing the button's centre found " << hit.dump(2);

    // Accepting the whole chain would also accept a materially wrong answer, so
    // pin the actual contract too: the element returned is the *smallest* one
    // covering the point, meaning nothing else covering it is smaller.
    const auto area = [](const json& element) -> int64_t {
        const auto& b = element["bounds"];
        return static_cast<int64_t>(b["width"].get<int>()) * b["height"].get<int>();
    };
    const auto hitArea = area(hit["element"]);
    EXPECT_LE(hitArea, area(button))
        << "hit_test returned something larger than the button it landed on";
    for (const auto& ancestorId : hit["ancestors"]) {
        auto ancestor = client.call_tool(
            "get_element_properties",
            json{{"session", session}, {"element", ancestorId.get<std::string>()}});
        if (!ancestor.contains("element"))
            continue;
        EXPECT_GE(area(ancestor["element"]), hitArea)
            << "an ancestor was smaller than the element hit_test chose, so it did not "
               "return the smallest element at the point";
    }

    // A point outside the window belongs to no element of this session.
    bool isError = false;
    client.call_tool("hit_test", json{{"session", session}, {"x", -30000}, {"y", -30000}},
                     &isError);
    EXPECT_TRUE(isError) << "a point outside the window must not resolve to an element";
}

TEST_F(McpSampleFixture, ScreenshotReturnsAnInlineImageOrWritesAFile) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto content = client.call_tool_content("screenshot", json{{"session", session}});
    ASSERT_FALSE(content.empty()) << "screenshot returned no content";

    bool sawImage = false;
    for (const auto& block : content) {
        if (block.value("type", "") != "image")
            continue;
        sawImage = true;
        EXPECT_EQ(block.value("mimeType", ""), "image/png");
        // Base64 of a real window capture; a few hundred bytes would mean an
        // empty or failed capture that still reported success.
        EXPECT_GT(block.value("data", "").size(), 1000u);
    }
    EXPECT_TRUE(sawImage) << "with no path the image must come back inline: " << content.dump();

    // The base64 must not also be repeated in the text summary, which would
    // double the cost of every screenshot for no benefit.
    for (const auto& block : content) {
        if (block.value("type", "") == "text")
            EXPECT_EQ(block.value("text", "").find("imageBase64"), std::string::npos);
    }

    const auto path = (fs::temp_directory_path() / "lvt_mcp_shot.png").string();
    std::error_code ec;
    fs::remove(path, ec);
    // Writing to a path is gated behind --allow-input, since it creates or
    // truncates the file, so this half needs a server that allows input.
    McpClient writer(true);
    ASSERT_TRUE(writer.started());
    ASSERT_TRUE(writer.handshake());
    const auto writeSession = connect(writer);
    ASSERT_FALSE(writeSession.empty());

    auto toFile = writer.call_tool(
        "screenshot", json{{"session", writeSession}, {"path", path}});
    EXPECT_EQ(toFile.value("path", ""), path) << toFile.dump(2);
    EXPECT_TRUE(fs::exists(path)) << "a path argument must write the PNG there";
    fs::remove(path, ec);
}

TEST_F(McpSampleFixture, GetElementPropertiesReadsWholeElementsAndNamedSubsets) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "ReadyCheckBox"}});
    ASSERT_EQ(found["elements"].size(), 1u) << found.dump(2);
    const auto id = found["elements"][0].value("id", "");

    auto whole = client.call_tool(
        "get_element_properties", json{{"session", session}, {"element", id}});
    ASSERT_TRUE(whole.contains("element")) << whole.dump(2);
    EXPECT_EQ(whole["element"].value("type", ""), "CheckBox");

    auto subset = client.call_tool(
        "get_element_properties",
        json{{"session", session}, {"element", id},
             {"properties", json::array({"Toggle.ToggleState", "AutomationId"})}});
    ASSERT_TRUE(subset.contains("properties")) << subset.dump(2);
    EXPECT_EQ(subset["properties"].value("AutomationId", ""), "ReadyCheckBox");
    EXPECT_FALSE(subset["properties"].value("Toggle.ToggleState", "").empty())
        << "a checkbox must report its toggle state";
}

TEST_F(McpSampleFixture, VisualTreeIsAvailableAndDiffersFromTheUiaTree) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto visual = client.call_tool("get_visual_tree", json{{"session", session}});
    ASSERT_TRUE(visual.contains("root")) << visual.dump(2);
    // The visual tree shows implementation structure — HWNDs and framework
    // types — that the UIA tree deliberately hides.
    EXPECT_NE(visual.dump().find("win32"), std::string::npos);
}

TEST_F(McpSampleFixture, TypedPropertySchemasAndDiffsReuseOnePersistentInjection) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto schemas = output_schemas(client);
    const auto session = connect(client, "visual");
    ASSERT_FALSE(session.empty());
    const auto targetPid = process_id();
    const int setSiteBefore = count_tap_set_site_calls(targetPid);
    const int getEnumsBefore = count_tap_get_enums_calls(targetPid);

    auto visual = client.call_tool("get_visual_tree", json{{"session", session}});
    ASSERT_TRUE(visual.contains("root")) << visual.dump(2);
    std::vector<const json*> elements;
    collect_json_elements(visual["root"], elements);
    const json* button = nullptr;
    const json* status = nullptr;
    for (const auto* element : elements) {
        const auto name =
            element->value("properties", json::object()).value("name", "");
        if (name == "PrimaryButton")
            button = element;
        else if (name == "StatusText")
            status = element;
    }

    const auto runUiaTypedProperties = [&] {
        SCOPED_TRACE("UIA typed properties are provider-driven and deterministic");
        SkipIfNotReady();
        McpClient client(true);
        ASSERT_TRUE(client.started());
        ASSERT_TRUE(client.handshake());
        const auto schemas = output_schemas(client);
        const auto session = connect(client);
        ASSERT_FALSE(session.empty());

        bool isError = false;
        auto input = client.call_tool(
            "find_elements", json{{"session", session}, {"automationId", "InputBox"}});
        ASSERT_EQ(input["elements"].size(), 1u) << input.dump(2);
        const auto inputKey = input["elements"][0].value("key", "");

        auto inputProperties = client.call_tool(
            "get_editable_properties",
            json{{"session", session}, {"element", inputKey}}, &isError);
        ASSERT_FALSE(isError) << inputProperties.dump(2);
        std::string schemaError;
        EXPECT_TRUE(schema_allows(
            schemas.at("get_editable_properties"), inputProperties, schemaError))
            << schemaError;
        const auto* valueDescriptor =
            find_property_descriptor(inputProperties, "Value.Value");
        ASSERT_NE(valueDescriptor, nullptr) << inputProperties.dump(2);
        EXPECT_EQ(valueDescriptor->value("kind", ""), "string");
        EXPECT_TRUE(valueDescriptor->value("writable", false));
        EXPECT_FALSE(valueDescriptor->value("supportsClear", true));
        const auto valueDescriptorId = valueDescriptor->value("descriptorId", "");
        const auto originalText =
            typed_property_value(inputProperties, "Value.Value");

        auto repeated = client.call_tool(
            "get_editable_properties",
            json{{"session", session}, {"element", inputKey}}, &isError);
        ASSERT_FALSE(isError) << repeated.dump(2);
        EXPECT_EQ(
            repeated.value("schemaId", ""),
            inputProperties.value("schemaId", ""));
        const auto* repeatedValue =
            find_property_descriptor(repeated, "Value.Value");
        ASSERT_NE(repeatedValue, nullptr);
        EXPECT_EQ(
            repeatedValue->value("descriptorId", ""), valueDescriptorId);

        const auto changedText = "typed UIA value";
        auto setValue = client.call_tool(
            "set_property",
            json{{"session", session},
                 {"element", inputKey},
                 {"descriptorId", valueDescriptorId},
                 {"value", changedText}},
            &isError);
        ASSERT_FALSE(isError) << setValue.dump(2);
        auto changedInput = client.call_tool(
            "get_editable_properties",
            json{{"session", session}, {"element", inputKey}}, &isError);
        ASSERT_FALSE(isError) << changedInput.dump(2);
        EXPECT_EQ(typed_property_value(changedInput, "Value.Value"), changedText);

        auto readOnly = client.call_tool(
            "find_elements", json{{"session", session}, {"automationId", "ReadOnlyBox"}});
        ASSERT_EQ(readOnly["elements"].size(), 1u) << readOnly.dump(2);
        auto readOnlyProperties = client.call_tool(
            "get_editable_properties",
            json{{"session", session},
                 {"element", readOnly["elements"][0].value("key", "")}},
            &isError);
        ASSERT_FALSE(isError) << readOnlyProperties.dump(2);
        const auto* readOnlyValue =
            find_property_descriptor(readOnlyProperties, "Value.Value");
        ASSERT_NE(readOnlyValue, nullptr);
        EXPECT_EQ(readOnlyValue->value("kind", ""), "readonly");
        EXPECT_FALSE(readOnlyValue->value("writable", true));

        auto slider = client.call_tool(
            "find_elements", json{{"session", session}, {"automationId", "LevelSlider"}});
        ASSERT_EQ(slider["elements"].size(), 1u) << slider.dump(2);
        const auto sliderKey = slider["elements"][0].value("key", "");
        auto sliderProperties = client.call_tool(
            "get_editable_properties",
            json{{"session", session}, {"element", sliderKey}}, &isError);
        ASSERT_FALSE(isError) << sliderProperties.dump(2);
        const auto* range =
            find_property_descriptor(sliderProperties, "RangeValue.Value");
        ASSERT_NE(range, nullptr) << sliderProperties.dump(2);
        EXPECT_EQ(range->value("kind", ""), "number");
        EXPECT_DOUBLE_EQ(range->value("minimum", -1.0), 0.0);
        EXPECT_DOUBLE_EQ(range->value("maximum", -1.0), 100.0);
        EXPECT_FALSE(range->contains("step"));
        const auto rangeDescriptorId = range->value("descriptorId", "");
        const auto originalRange =
            typed_property_value(sliderProperties, "RangeValue.Value");

        auto readOnlyRange = client.call_tool(
            "find_elements",
            json{{"session", session}, {"automationId", "ReadOnlyRange"}});
        ASSERT_EQ(readOnlyRange["elements"].size(), 1u) << readOnlyRange.dump(2);
        auto readOnlyRangeProperties = client.call_tool(
            "get_editable_properties",
            json{{"session", session},
                 {"element", readOnlyRange["elements"][0].value("key", "")}},
            &isError);
        ASSERT_FALSE(isError) << readOnlyRangeProperties.dump(2);
        const auto* readOnlyRangeDescriptor =
            find_property_descriptor(
                readOnlyRangeProperties, "RangeValue.Value");
        ASSERT_NE(readOnlyRangeDescriptor, nullptr)
            << readOnlyRangeProperties.dump(2);
        EXPECT_EQ(readOnlyRangeDescriptor->value("kind", ""), "readonly");
        EXPECT_FALSE(readOnlyRangeDescriptor->value("writable", true));
        EXPECT_DOUBLE_EQ(
            readOnlyRangeDescriptor->value("minimum", -1.0), 0.0);
        EXPECT_DOUBLE_EQ(
            readOnlyRangeDescriptor->value("maximum", -1.0), 100.0);

        const auto requestedRange = "075.1300";
        auto setRange = client.call_tool(
            "set_property",
            json{{"session", session},
                 {"element", sliderKey},
                 {"descriptorId", rangeDescriptorId},
                 {"value", requestedRange}},
            &isError);
        ASSERT_FALSE(isError) << setRange.dump(2);
        const auto providerRange = setRange.value("value", "");
        ASSERT_FALSE(providerRange.empty()) << setRange.dump(2);
        EXPECT_NE(providerRange, requestedRange)
            << "RangeValue mutation returned the caller's spelling instead of provider readback";
        auto rangeAfter = client.call_tool(
            "get_editable_properties",
            json{{"session", session}, {"element", sliderKey}}, &isError);
        ASSERT_FALSE(isError) << rangeAfter.dump(2);
        EXPECT_EQ(
            typed_property_value(rangeAfter, "RangeValue.Value"), providerRange)
            << rangeAfter.dump(2);

        auto checkbox = client.call_tool(
            "find_elements", json{{"session", session}, {"automationId", "ReadyCheckBox"}});
        ASSERT_EQ(checkbox["elements"].size(), 1u) << checkbox.dump(2);
        const auto checkboxKey = checkbox["elements"][0].value("key", "");
        auto checkboxProperties = client.call_tool(
            "get_editable_properties",
            json{{"session", session}, {"element", checkboxKey}}, &isError);
        ASSERT_FALSE(isError) << checkboxProperties.dump(2);
        const auto* toggle =
            find_property_descriptor(checkboxProperties, "Toggle.ToggleState");
        ASSERT_NE(toggle, nullptr) << checkboxProperties.dump(2);
        EXPECT_EQ(toggle->value("kind", ""), "enum");
        EXPECT_TRUE(descriptor_has_choice(*toggle, "Off"));
        EXPECT_TRUE(descriptor_has_choice(*toggle, "On"));
        EXPECT_FALSE(descriptor_has_choice(*toggle, "Indeterminate"));
        const auto toggleDescriptorId = toggle->value("descriptorId", "");
        const auto originalToggle =
            typed_property_value(checkboxProperties, "Toggle.ToggleState");
        const auto wantedToggle = originalToggle == "On" ? "Off" : "On";
        for (int attempt = 0; attempt < 2; ++attempt) {
            auto setToggle = client.call_tool(
                "set_property",
                json{{"session", session},
                     {"element", checkboxKey},
                     {"descriptorId", toggleDescriptorId},
                     {"value", wantedToggle}},
                &isError);
            ASSERT_FALSE(isError) << setToggle.dump(2);
        }
        auto toggleAfter = client.call_tool(
            "get_editable_properties",
            json{{"session", session}, {"element", checkboxKey}}, &isError);
        ASSERT_FALSE(isError) << toggleAfter.dump(2);
        EXPECT_EQ(
            typed_property_value(toggleAfter, "Toggle.ToggleState"), wantedToggle)
            << "setting the same toggle state twice must not cycle away from it\n"
            << toggleAfter.dump(2);

        auto combo = client.call_tool(
            "find_elements", json{{"session", session}, {"automationId", "ChoiceCombo"}});
        ASSERT_EQ(combo["elements"].size(), 1u) << combo.dump(2);
        auto comboProperties = client.call_tool(
            "get_editable_properties",
            json{{"session", session},
                 {"element", combo["elements"][0].value("key", "")}},
            &isError);
        ASSERT_FALSE(isError) << comboProperties.dump(2);
        const auto* expand =
            find_property_descriptor(comboProperties, "ExpandCollapse.State");
        ASSERT_NE(expand, nullptr) << comboProperties.dump(2);
        EXPECT_TRUE(descriptor_has_choice(*expand, "Expanded"));
        EXPECT_TRUE(descriptor_has_choice(*expand, "Collapsed"));
        auto setExpanded = client.call_tool(
            "set_property",
            json{{"session", session},
                 {"element", combo["elements"][0].value("key", "")},
                 {"descriptorId", expand->value("descriptorId", "")},
                 {"value", "Expanded"}},
            &isError);
        ASSERT_FALSE(isError) << setExpanded.dump(2);
        auto expandedCombo = client.call_tool(
            "find_elements", json{{"session", session}, {"automationId", "ChoiceCombo"}});
        ASSERT_FALSE(expandedCombo["elements"].empty());
        auto expandedProperties = client.call_tool(
            "get_editable_properties",
            json{{"session", session},
                 {"element", expandedCombo["elements"][0].value("key", "")}},
            &isError);
        ASSERT_FALSE(isError) << expandedProperties.dump(2);
        EXPECT_EQ(
            typed_property_value(expandedProperties, "ExpandCollapse.State"),
            "Expanded");
        const auto* collapse =
            find_property_descriptor(expandedProperties, "ExpandCollapse.State");
        ASSERT_NE(collapse, nullptr);
        auto setCollapsed = client.call_tool(
            "set_property",
            json{{"session", session},
                 {"element", expandedCombo["elements"][0].value("key", "")},
                 {"descriptorId", collapse->value("descriptorId", "")},
                 {"value", "Collapsed"}},
            &isError);
        ASSERT_FALSE(isError) << setCollapsed.dump(2);

        auto item = client.call_tool(
            "find_elements",
            json{{"session", session}, {"name", "Item 000"}, {"type", "ListItem"}});
        if (!item["elements"].empty()) {
            const auto itemKey = item["elements"][0].value("key", "");
            auto itemProperties = client.call_tool(
                "get_editable_properties",
                json{{"session", session}, {"element", itemKey}}, &isError);
            ASSERT_FALSE(isError) << itemProperties.dump(2);
            const auto* selection =
                find_property_descriptor(itemProperties, "SelectionItem.Action");
            ASSERT_NE(selection, nullptr) << itemProperties.dump(2);
            EXPECT_EQ(selection->value("kind", ""), "command");
            EXPECT_TRUE(descriptor_has_choice(*selection, "select"));
            EXPECT_TRUE(descriptor_has_choice(*selection, "add"));
            EXPECT_TRUE(descriptor_has_choice(*selection, "remove"));
            auto add = client.call_tool(
                "set_property",
                json{{"session", session},
                     {"element", itemKey},
                     {"descriptorId", selection->value("descriptorId", "")},
                     {"value", "add"}},
                &isError);
            ASSERT_FALSE(isError) << add.dump(2);

            auto currentItem = client.call_tool(
                "find_elements",
                json{{"session", session}, {"name", "Item 000"}, {"type", "ListItem"}});
            ASSERT_FALSE(currentItem["elements"].empty());
            auto selectedProperties = client.call_tool(
                "get_editable_properties",
                json{{"session", session},
                     {"element", currentItem["elements"][0].value("key", "")}},
                &isError);
            ASSERT_FALSE(isError) << selectedProperties.dump(2);
            EXPECT_EQ(
                typed_property_value(selectedProperties, "SelectionItem.Action"),
                "Selected");
            const auto* remove =
                find_property_descriptor(selectedProperties, "SelectionItem.Action");
            ASSERT_NE(remove, nullptr);
            auto removed = client.call_tool(
                "set_property",
                json{{"session", session},
                     {"element", currentItem["elements"][0].value("key", "")},
                     {"descriptorId", remove->value("descriptorId", "")},
                     {"value", "remove"}},
                &isError);
            ASSERT_FALSE(isError) << removed.dump(2);

            auto replacementItem = client.call_tool(
                "find_elements",
                json{{"session", session}, {"name", "Item 000"}, {"type", "ListItem"}});
            ASSERT_FALSE(replacementItem["elements"].empty());
            auto replacementProperties = client.call_tool(
                "get_editable_properties",
                json{{"session", session},
                     {"element", replacementItem["elements"][0].value("key", "")}},
                &isError);
            ASSERT_FALSE(isError) << replacementProperties.dump(2);
            const auto* replace =
                find_property_descriptor(replacementProperties, "SelectionItem.Action");
            ASSERT_NE(replace, nullptr);
            auto selected = client.call_tool(
                "set_property",
                json{{"session", session},
                     {"element", replacementItem["elements"][0].value("key", "")},
                     {"descriptorId", replace->value("descriptorId", "")},
                     {"value", "select"}},
                &isError);
            ASSERT_FALSE(isError) << selected.dump(2);

            auto selectedItem = client.call_tool(
                "find_elements",
                json{{"session", session}, {"name", "Item 000"}, {"type", "ListItem"}});
            ASSERT_FALSE(selectedItem["elements"].empty());
            auto selectedSnapshot = client.call_tool(
                "get_editable_properties",
                json{{"session", session},
                     {"element", selectedItem["elements"][0].value("key", "")}},
                &isError);
            ASSERT_FALSE(isError) << selectedSnapshot.dump(2);
            const auto* selectedDescriptor =
                find_property_descriptor(selectedSnapshot, "SelectionItem.Action");
            ASSERT_NE(selectedDescriptor, nullptr);
            auto finalRemove = client.call_tool(
                "set_property",
                json{{"session", session},
                     {"element", selectedItem["elements"][0].value("key", "")},
                     {"descriptorId", selectedDescriptor->value("descriptorId", "")},
                     {"value", "remove"}},
                &isError);
            EXPECT_FALSE(isError) << finalRemove.dump(2);
        }

        auto list = client.call_tool(
            "find_elements", json{{"session", session}, {"automationId", "ItemsList"}});
        ASSERT_EQ(list["elements"].size(), 1u) << list.dump(2);
        const auto listKey = list["elements"][0].value("key", "");
        auto listProperties = client.call_tool(
            "get_editable_properties",
            json{{"session", session}, {"element", listKey}}, &isError);
        ASSERT_FALSE(isError) << listProperties.dump(2);
        const auto* scroll =
            find_property_descriptor(listProperties, "Scroll.Action");
        ASSERT_NE(scroll, nullptr) << listProperties.dump(2);
        EXPECT_EQ(scroll->value("kind", ""), "command");
        EXPECT_TRUE(descriptor_has_choice(*scroll, "up"));
        EXPECT_TRUE(descriptor_has_choice(*scroll, "down"));
        EXPECT_FALSE(descriptor_has_choice(*scroll, "left"));
        EXPECT_FALSE(descriptor_has_choice(*scroll, "right"));
        const auto beforeScroll = typed_property_value(listProperties, "Scroll.Action");
        auto scrolled = client.call_tool(
            "set_property",
            json{{"session", session},
                 {"element", listKey},
                 {"descriptorId", scroll->value("descriptorId", "")},
                 {"value", "down"}},
            &isError);
        ASSERT_FALSE(isError) << scrolled.dump(2);
        auto afterScroll = client.call_tool(
            "get_editable_properties",
            json{{"session", session}, {"element", listKey}}, &isError);
        ASSERT_FALSE(isError) << afterScroll.dump(2);
        EXPECT_NE(
            typed_property_value(afterScroll, "Scroll.Action"), beforeScroll);

        auto clear = client.call_tool(
            "clear_property",
            json{{"session", session},
                 {"element", sliderKey},
                 {"descriptorId", rangeDescriptorId}},
            &isError);
        EXPECT_TRUE(isError) << clear.dump(2);
        EXPECT_NE(clear.dump().find("does not expose a generic clear"), std::string::npos)
            << clear.dump(2);

        auto restoreRange = client.call_tool(
            "set_property",
            json{{"session", session},
                 {"element", sliderKey},
                 {"descriptorId", rangeDescriptorId},
                 {"value", originalRange}},
            &isError);
        EXPECT_FALSE(isError) << restoreRange.dump(2);
        auto restoreToggle = client.call_tool(
            "set_property",
            json{{"session", session},
                 {"element", checkboxKey},
                 {"descriptorId", toggleDescriptorId},
                 {"value", originalToggle}},
            &isError);
        EXPECT_FALSE(isError) << restoreToggle.dump(2);
        auto restoreText = client.call_tool(
            "set_property",
            json{{"session", session},
                 {"element", inputKey},
                 {"descriptorId", valueDescriptorId},
                 {"value", originalText}},
            &isError);
        EXPECT_FALSE(isError) << restoreText.dump(2);
    };

    const auto runUiaResourceRefresh = [&] {
        SCOPED_TRACE("UIA typed property mutation refreshes subscribed resource");
        SkipIfNotReady();
        McpClient client(true);
        ASSERT_TRUE(client.started());
        ASSERT_TRUE(client.handshake());
        const auto session = connect(client);
        ASSERT_FALSE(session.empty());

        auto input = client.call_tool(
            "find_elements", json{{"session", session}, {"automationId", "InputBox"}});
        ASSERT_EQ(input["elements"].size(), 1u) << input.dump(2);
        const auto inputKey = input["elements"][0].value("key", "");

        bool isError = false;
        auto properties = client.call_tool(
            "get_editable_properties",
            json{{"session", session}, {"element", inputKey}}, &isError);
        ASSERT_FALSE(isError) << properties.dump(2);
        const auto* value =
            find_property_descriptor(properties, "Value.Value");
        ASSERT_NE(value, nullptr);
        const auto descriptorId = value->value("descriptorId", "");
        const auto original = typed_property_value(properties, "Value.Value");

        const auto uri = "lvt://session/" + session + "/uia-tree";
        auto subscribed = client.request("resources/subscribe", json{{"uri", uri}});
        ASSERT_TRUE(subscribed.contains("result")) << subscribed.dump(2);
        auto initialRead = client.request("resources/read", json{{"uri", uri}});
        ASSERT_TRUE(initialRead.contains("result")) << initialRead.dump(2);

        auto changed = client.call_tool(
            "set_property",
            json{{"session", session},
                 {"element", inputKey},
                 {"descriptorId", descriptorId},
                 {"value", "typed resource mutation"}},
            &isError);
        ASSERT_FALSE(isError) << changed.dump(2);

        auto notification =
            client.await_notification("notifications/resources/updated", 20);
        ASSERT_FALSE(notification.is_null())
            << "no resource update followed a generic UIA property mutation";
        auto read = client.request("resources/read", json{{"uri", uri}});
        ASSERT_TRUE(read.contains("result")) << read.dump(2);
        const auto patch = json::parse(
            read["result"]["contents"][0].value("text", ""), nullptr, false);
        ASSERT_FALSE(patch.is_discarded()) << read.dump(2);
        bool sawValueChange = false;
        for (const auto& event : patch.value("events", json::array())) {
            const auto fields = event.value("fields", json::object());
            sawValueChange = sawValueChange ||
                             (event.value("key", "") == inputKey &&
                              fields.contains("properties.Value.Value"));
        }
        EXPECT_TRUE(sawValueChange) << patch.dump(2);

        auto unsubscribed = client.request("resources/unsubscribe", json{{"uri", uri}});
        EXPECT_TRUE(unsubscribed.contains("result")) << unsubscribed.dump(2);
        auto restored = client.call_tool(
            "set_property",
            json{{"session", session},
                 {"element", inputKey},
                 {"descriptorId", descriptorId},
                 {"value", original}},
            &isError);
        EXPECT_FALSE(isError) << restored.dump(2);
    };
    ASSERT_NE(button, nullptr);
    ASSERT_NE(status, nullptr);
    const auto key = button->value("key", "");
    const auto statusKey = status->value("key", "");
    ASSERT_EQ(key.rfind("winui3:0x", 0), 0u);
    ASSERT_EQ(statusKey.rfind("winui3:0x", 0), 0u);

    bool isError = false;
    auto properties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", key}}, &isError);
    ASSERT_FALSE(isError) << properties.dump(2);
    ASSERT_TRUE(properties.contains("descriptors")) << properties.dump(2);
    ASSERT_TRUE(properties.contains("values")) << properties.dump(2);
    ASSERT_FALSE(properties.value("schemaId", "").empty()) << properties.dump(2);
    std::string schemaError;
    EXPECT_TRUE(schema_allows(
        schemas.at("get_editable_properties"), properties, schemaError)) << schemaError;

    const auto* opacity = find_property_descriptor(properties, "Opacity");
    ASSERT_NE(opacity, nullptr) << "PrimaryButton did not report Opacity";
    EXPECT_TRUE(opacity->value("writable", false));
    EXPECT_EQ(opacity->value("kind", ""), "number");
    const auto opacityDescriptorId = opacity->value("descriptorId", "");
    ASSERT_FALSE(opacityDescriptorId.empty());
    EXPECT_FALSE(opacity->contains("propertyIndex"));
    EXPECT_FALSE(opacity->contains("valueType"));

    auto repeatedProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", key}}, &isError);
    ASSERT_FALSE(isError) << repeatedProperties.dump(2);
    EXPECT_EQ(
        repeatedProperties.value("schemaId", ""),
        properties.value("schemaId", ""));
    const auto* repeatedOpacity =
        find_property_descriptor(repeatedProperties, "Opacity");
    ASSERT_NE(repeatedOpacity, nullptr);
    EXPECT_EQ(
        repeatedOpacity->value("descriptorId", ""), opacityDescriptorId);

    auto initialChanges = client.call_tool(
        "get_visual_tree_changes", json{{"session", session}}, &isError);
    ASSERT_FALSE(isError) << initialChanges.dump(2);
    EXPECT_TRUE(initialChanges.value("snapshot", false));
    EXPECT_FALSE(initialChanges.value("events", json::array()).empty());
    schemaError.clear();
    EXPECT_TRUE(schema_allows(
        schemas.at("get_visual_tree_changes"), initialChanges, schemaError)) << schemaError;

    auto statusProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", statusKey}}, &isError);
    ASSERT_FALSE(isError) << statusProperties.dump(2);
    const auto* textProperty =
        find_property_descriptor(statusProperties, "Text");
    ASSERT_NE(textProperty, nullptr) << "StatusText did not report Text";
    EXPECT_TRUE(textProperty->value("propertyType", "").ends_with("String"));
    EXPECT_EQ(textProperty->value("kind", ""), "string")
        << "TextBlock.Text must be classified from PropertyChainValue.Type, "
           "not the current value's runtime ValueType";
    const auto textDescriptorId =
        textProperty->value("descriptorId", "");
    ASSERT_FALSE(textDescriptorId.empty());
    const auto originalText = typed_property_value(statusProperties, "Text");
    const auto changedText = "mcp \"quoted\"\nvalue";

    const auto* textAlignment =
        find_property_descriptor(statusProperties, "TextAlignment");
    ASSERT_NE(textAlignment, nullptr)
        << "StatusText did not report TextAlignment";
    EXPECT_EQ(textAlignment->value("kind", ""), "enum");
    const auto alignmentDescriptorId =
        textAlignment->value("descriptorId", "");
    ASSERT_FALSE(alignmentDescriptorId.empty());
    const auto alignmentChoices =
        textAlignment->value("choices", json::array());
    ASSERT_FALSE(alignmentChoices.empty()) << textAlignment->dump(2);
    std::string centerValue;
    for (const auto& choice : alignmentChoices) {
        if (choice.value("label", "") == "Center") {
            centerValue = choice.value("value", "");
            break;
        }
    }
    ASSERT_FALSE(centerValue.empty()) << alignmentChoices.dump(2);
    const auto originalAlignment =
        typed_property_value(statusProperties, "TextAlignment");
    ASSERT_FALSE(originalAlignment.empty());

    auto setAlignment = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", alignmentDescriptorId},
             {"value", centerValue}},
        &isError);
    ASSERT_FALSE(isError) << setAlignment.dump(2);
    EXPECT_EQ(setAlignment.value("value", ""), centerValue)
        << "enum mutation must return canonical effective readback, not raw numeric storage";
    EXPECT_FALSE(setAlignment.value("runtimeType", "").empty());
    EXPECT_FALSE(setAlignment.value("overridden", true));
    EXPECT_TRUE(setAlignment.value("canClear", false));
    EXPECT_NE(setAlignment.value("source", "").find("Local"), std::string::npos);
    auto centeredProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", statusKey}}, &isError);
    ASSERT_FALSE(isError) << centeredProperties.dump(2);
    EXPECT_EQ(
        typed_property_value(centeredProperties, "TextAlignment"),
        centerValue);
    EXPECT_EQ(
        centeredProperties.value("schemaId", ""),
        statusProperties.value("schemaId", ""));
    const auto* centeredAlignment =
        find_property_descriptor(centeredProperties, "TextAlignment");
    ASSERT_NE(centeredAlignment, nullptr);
    EXPECT_EQ(
        centeredAlignment->value("descriptorId", ""),
        alignmentDescriptorId);

    auto invalidAlignment = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", alignmentDescriptorId},
             {"value", "__not_a_text_alignment__"}},
        &isError);
    EXPECT_TRUE(isError) << invalidAlignment.dump(2);
    auto afterInvalidAlignment = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", statusKey}}, &isError);
    ASSERT_FALSE(isError) << afterInvalidAlignment.dump(2);
    EXPECT_EQ(
        typed_property_value(afterInvalidAlignment, "TextAlignment"),
        centerValue);

    auto restoreAlignment = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", alignmentDescriptorId},
             {"value", originalAlignment}},
        &isError);
    ASSERT_FALSE(isError) << restoreAlignment.dump(2);

    const auto* manipulationMode =
        find_property_descriptor(statusProperties, "ManipulationMode");
    ASSERT_NE(manipulationMode, nullptr)
        << "StatusText did not report ManipulationMode";
    EXPECT_EQ(manipulationMode->value("kind", ""), "enum");
    const auto manipulationDescriptorId =
        manipulationMode->value("descriptorId", "");
    ASSERT_FALSE(manipulationDescriptorId.empty());
    const auto manipulationChoices =
        manipulationMode->value("choices", json::array());
    bool hasTranslateX = false;
    bool hasScale = false;
    for (const auto& choice : manipulationChoices) {
        hasTranslateX =
            hasTranslateX || choice.value("value", "") == "TranslateX";
        hasScale = hasScale || choice.value("value", "") == "Scale";
    }
    ASSERT_TRUE(hasTranslateX) << manipulationChoices.dump(2);
    ASSERT_TRUE(hasScale) << manipulationChoices.dump(2);
    const auto originalManipulationMode =
        typed_property_value(statusProperties, "ManipulationMode");
    ASSERT_FALSE(originalManipulationMode.empty());

    auto setManipulationMode = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", manipulationDescriptorId},
             {"value", " TranslateX , Scale "}},
        &isError);
    ASSERT_FALSE(isError) << setManipulationMode.dump(2);
    EXPECT_EQ(
        setManipulationMode.value("value", ""),
        "TranslateX,Scale");
    auto compositeProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", statusKey}}, &isError);
    ASSERT_FALSE(isError) << compositeProperties.dump(2);
    EXPECT_EQ(
        typed_property_value(compositeProperties, "ManipulationMode"),
        "TranslateX,Scale");

    auto invalidManipulationMode = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", manipulationDescriptorId},
             {"value", "TranslateX,NotAFlag"}},
        &isError);
    EXPECT_TRUE(isError) << invalidManipulationMode.dump(2);
    auto afterInvalidManipulation = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", statusKey}}, &isError);
    ASSERT_FALSE(isError) << afterInvalidManipulation.dump(2);
    EXPECT_EQ(
        typed_property_value(
            afterInvalidManipulation, "ManipulationMode"),
        "TranslateX,Scale");

    auto restoreManipulationMode = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", manipulationDescriptorId},
             {"value", originalManipulationMode}},
        &isError);
    ASSERT_FALSE(isError) << restoreManipulationMode.dump(2);

    const auto* textDecorations =
        find_property_descriptor(statusProperties, "TextDecorations");
    ASSERT_NE(textDecorations, nullptr)
        << "StatusText did not report TextDecorations";
    EXPECT_EQ(textDecorations->value("kind", ""), "enum");
    const auto decorationsDescriptorId =
        textDecorations->value("descriptorId", "");
    ASSERT_FALSE(decorationsDescriptorId.empty());
    const auto decorationChoices =
        textDecorations->value("choices", json::array());
    std::string expectedDecorations;
    bool hasUnderline = false;
    bool hasStrikethrough = false;
    for (const auto& choice : decorationChoices) {
        const auto choiceValue = choice.value("value", "");
        if (choiceValue != "Underline" &&
            choiceValue != "Strikethrough") {
            continue;
        }
        hasUnderline = hasUnderline || choiceValue == "Underline";
        hasStrikethrough =
            hasStrikethrough || choiceValue == "Strikethrough";
        if (!expectedDecorations.empty())
            expectedDecorations += ",";
        expectedDecorations += choiceValue;
    }
    ASSERT_TRUE(hasUnderline) << decorationChoices.dump(2);
    ASSERT_TRUE(hasStrikethrough) << decorationChoices.dump(2);
    const auto originalDecorations =
        typed_property_value(statusProperties, "TextDecorations");
    ASSERT_FALSE(originalDecorations.empty());

    auto setDecorations = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", decorationsDescriptorId},
             {"value", " Strikethrough , Underline "}},
        &isError);
    ASSERT_FALSE(isError) << setDecorations.dump(2);
    EXPECT_EQ(
        setDecorations.value("value", ""), expectedDecorations);
    auto decoratedProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", statusKey}}, &isError);
    ASSERT_FALSE(isError) << decoratedProperties.dump(2);
    EXPECT_EQ(
        typed_property_value(decoratedProperties, "TextDecorations"),
        expectedDecorations);

    auto invalidDecorations = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", decorationsDescriptorId},
             {"value", "Underline,NotADecoration"}},
        &isError);
    EXPECT_TRUE(isError) << invalidDecorations.dump(2);
    auto afterInvalidDecorations = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", statusKey}}, &isError);
    ASSERT_FALSE(isError) << afterInvalidDecorations.dump(2);
    EXPECT_EQ(
        typed_property_value(
            afterInvalidDecorations, "TextDecorations"),
        expectedDecorations);

    auto restoreDecorations = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", decorationsDescriptorId},
             {"value", originalDecorations}},
        &isError);
    ASSERT_FALSE(isError) << restoreDecorations.dump(2);

    auto unknown = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", key},
             {"descriptorId", "not-a-real-descriptor"},
             {"value", "0.5"}},
        &isError);
    EXPECT_TRUE(isError) << unknown.dump(2);

    auto mismatched = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", opacityDescriptorId},
             {"value", "0.5"}},
        &isError);
    EXPECT_TRUE(isError) << mismatched.dump(2);

    auto clientMetadata = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", key},
             {"descriptorId", opacityDescriptorId},
             {"propertyIndex", 1},
             {"valueType", "String"},
             {"value", "0.5"}},
        &isError);
    EXPECT_TRUE(isError) << clientMetadata.dump(2);

    auto set = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", key},
             {"descriptorId", opacityDescriptorId},
             {"value", "0.5000"}},
        &isError);
    ASSERT_FALSE(isError) << set.dump(2);
    EXPECT_TRUE(set.value("ok", false));
    ASSERT_TRUE(set.contains("value")) << set.dump(2);
    EXPECT_NE(set.value("value", ""), "0.5000")
        << "set_property must return the framework's effective formatting, not echo input";
    EXPECT_NEAR(std::stod(set.value("value", "")), 0.5, 0.001);
    EXPECT_FALSE(set.value("runtimeType", "").empty());
    EXPECT_FALSE(set.value("overridden", true));
    EXPECT_TRUE(set.value("canClear", false));
    EXPECT_NE(set.value("source", "").find("Local"), std::string::npos);
    schemaError.clear();
    EXPECT_TRUE(schema_allows(
        schemas.at("set_property"), set, schemaError)) << schemaError;

    auto setText = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", textDescriptorId},
             {"value", changedText}},
        &isError);
    ASSERT_FALSE(isError) << setText.dump(2);
    auto changedStatus = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", statusKey}}, &isError);
    ASSERT_FALSE(isError) << changedStatus.dump(2);

    auto changedProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", key}}, &isError);
    ASSERT_FALSE(isError) << changedProperties.dump(2);
    ASSERT_FALSE(typed_property_value(changedProperties, "Opacity").empty());
    EXPECT_NEAR(
        std::stod(typed_property_value(changedProperties, "Opacity")), 0.5, 0.001);
    EXPECT_EQ(typed_property_value(changedStatus, "Text"), changedText)
        << "spaces, quotes, and newlines must survive the persistent TAP protocol";

    auto changes = client.call_tool(
        "get_visual_tree_changes", json{{"session", session}}, &isError);
    ASSERT_FALSE(isError) << changes.dump(2);
    EXPECT_FALSE(changes.value("snapshot", true));
    bool sawTextChange = false;
    for (const auto& event : changes.value("events", json::array())) {
        const auto fields = event.value("fields", json::object());
        sawTextChange = sawTextChange || fields.contains("text") ||
                        fields.contains("properties.Text");
    }
    EXPECT_TRUE(sawTextChange) << changes.dump(2);

    auto clear = client.call_tool(
        "clear_property",
        json{{"session", session},
             {"element", key},
             {"descriptorId", opacityDescriptorId}},
        &isError);
    ASSERT_FALSE(isError) << clear.dump(2);
    EXPECT_TRUE(clear.value("cleared", false));
    ASSERT_TRUE(clear.contains("value")) << clear.dump(2);
    EXPECT_NEAR(std::stod(clear.value("value", "")), 1.0, 0.001);
    EXPECT_FALSE(clear.value("runtimeType", "").empty());
    EXPECT_FALSE(clear.value("overridden", true));
    EXPECT_FALSE(clear.value("canClear", true));
    EXPECT_FALSE(clear.value("source", "").empty());
    schemaError.clear();
    EXPECT_TRUE(schema_allows(
        schemas.at("clear_property"), clear, schemaError)) << schemaError;

    auto restoreText = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", textDescriptorId},
             {"value", originalText}},
        &isError);
    ASSERT_FALSE(isError) << restoreText.dump(2);

    auto clearedProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", key}}, &isError);
    ASSERT_FALSE(isError) << clearedProperties.dump(2);
    ASSERT_FALSE(typed_property_value(clearedProperties, "Opacity").empty());
    EXPECT_NEAR(
        std::stod(typed_property_value(clearedProperties, "Opacity")), 1.0, 0.001);
    EXPECT_EQ(
        typed_property_value(clearedProperties, "Opacity"),
        clear.value("value", ""))
        << "clear_property readback must match the subsequent property snapshot";

    auto afterClear = client.call_tool(
        "get_visual_tree_changes", json{{"session", session}}, &isError);
    ASSERT_FALSE(isError) << afterClear.dump(2);
    EXPECT_FALSE(afterClear.value("snapshot", true));

    auto disconnected = client.call_tool(
        "disconnect", json{{"session", session}}, &isError);
    ASSERT_FALSE(isError) << disconnected.dump(2);
    EXPECT_EQ(count_tap_set_site_calls(targetPid) - setSiteBefore, 1)
        << "one MCP session must reuse one TAP injection across tree reads and "
           "get/set/clear property operations";
    EXPECT_EQ(count_tap_get_enums_calls(targetPid) - getEnumsBefore, 1)
        << "one persistent XAML connection must fetch its enum catalog once";
    runUiaTypedProperties();
    runUiaResourceRefresh();
}

TEST_F(NativeMcpFixture, NativeTypedPropertiesUseGenericContractAndInputGate) {
    McpClient readOnly(false);
    ASSERT_TRUE(readOnly.started());
    ASSERT_TRUE(readOnly.handshake());
    const auto readOnlySession = connect(readOnly);
    ASSERT_FALSE(readOnlySession.empty());

    auto readOnlyTree = readOnly.call_tool(
        "get_visual_tree", json{{"session", readOnlySession}});
    ASSERT_TRUE(readOnlyTree.contains("root")) << readOnlyTree.dump(2);
    const auto* readOnlyGeneric = find_by_class(
        readOnlyTree["root"], "LvtNativePropertyFixtureText");
    ASSERT_NE(readOnlyGeneric, nullptr);
    const auto readOnlyKey = readOnlyGeneric->value("key", "");
    ASSERT_EQ(readOnlyKey.rfind("win32:0x", 0), 0u);

    bool isError = false;
    auto readOnlyProperties = readOnly.call_tool(
        "get_editable_properties",
        json{{"session", readOnlySession}, {"element", readOnlyKey}},
        &isError);
    ASSERT_FALSE(isError) << readOnlyProperties.dump(2);
    EXPECT_EQ(
        typed_property_value(readOnlyProperties, "Text"),
        "Generic child seed");

    auto refused = readOnly.call_tool(
        "set_property",
        json{{"session", readOnlySession},
             {"element", readOnlyKey},
             {"descriptorId",
              find_property_descriptor(readOnlyProperties, "Text")
                  ->value("descriptorId", "")},
             {"value", "must not be written"}},
        &isError);
    EXPECT_TRUE(isError) << refused.dump(2);

    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto schemas = output_schemas(client);
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto tree = client.call_tool(
        "get_visual_tree", json{{"session", session}});
    ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
    const auto* generic =
        find_by_class(tree["root"], "LvtNativePropertyFixtureText");
    const auto* combo =
        find_by_class(tree["root"], "ComboBox");
    const auto* status =
        find_by_class(tree["root"], "msctls_statusbar32");
    ASSERT_NE(generic, nullptr);
    ASSERT_NE(combo, nullptr);
    ASSERT_NE(status, nullptr);

    const auto genericKey = generic->value("key", "");
    auto properties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", genericKey}},
        &isError);
    ASSERT_FALSE(isError) << properties.dump(2);
    std::string schemaError;
    EXPECT_TRUE(schema_allows(
        schemas.at("get_editable_properties"), properties, schemaError))
        << schemaError;
    const auto* text = find_property_descriptor(properties, "Text");
    const auto* enabled = find_property_descriptor(properties, "Enabled");
    ASSERT_NE(text, nullptr);
    ASSERT_NE(enabled, nullptr);
    EXPECT_EQ(text->value("kind", ""), "string");
    EXPECT_EQ(enabled->value("kind", ""), "boolean");
    EXPECT_TRUE(text->value("writable", false));
    EXPECT_FALSE(text->contains("message"));
    EXPECT_FALSE(text->contains("wParam"));
    EXPECT_FALSE(text->contains("lParam"));

    auto repeated = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", genericKey}},
        &isError);
    ASSERT_FALSE(isError) << repeated.dump(2);
    EXPECT_EQ(
        repeated.value("schemaId", ""),
        properties.value("schemaId", ""));
    EXPECT_EQ(
        find_property_descriptor(repeated, "Text")
            ->value("descriptorId", ""),
        text->value("descriptorId", ""));

    const auto changedText =
        std::string(reinterpret_cast<const char*>(u8"MCP ✓ 東京"));
    auto changed = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", genericKey},
             {"descriptorId", text->value("descriptorId", "")},
             {"value", changedText}},
        &isError);
    ASSERT_FALSE(isError) << changed.dump(2);
    EXPECT_EQ(changed.value("value", ""), changedText);
    schemaError.clear();
    EXPECT_TRUE(schema_allows(
        schemas.at("set_property"), changed, schemaError))
        << schemaError;

    auto changedProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", genericKey}},
        &isError);
    ASSERT_FALSE(isError) << changedProperties.dump(2);
    EXPECT_EQ(
        typed_property_value(changedProperties, "Text"), changedText);

    for (const std::string value : {"", "Generic child seed"}) {
        auto setText = client.call_tool(
            "set_property",
            json{{"session", session},
                 {"element", genericKey},
                 {"descriptorId", text->value("descriptorId", "")},
                 {"value", value}},
            &isError);
        ASSERT_FALSE(isError) << setText.dump(2);
        EXPECT_EQ(setText.value("value", ""), value);
    }

    auto unknown = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", genericKey},
             {"descriptorId", "native-v1:not-a-descriptor"},
             {"value", "value"}},
        &isError);
    EXPECT_TRUE(isError) << unknown.dump(2);

    DWORD_PTR siblingValue = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kGetOutOfTreeHwndMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
            &siblingValue),
        0);
    char siblingRef[64]{};
    snprintf(
        siblingRef, sizeof(siblingRef), "win32:0x%llX",
        static_cast<unsigned long long>(siblingValue));
    auto outOfTree = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", siblingRef}},
        &isError);
    EXPECT_TRUE(isError) << outOfTree.dump(2);
    EXPECT_NE(
        outOfTree.value("error", "").find("unknown"),
        std::string::npos);

    const auto comboKey = combo->value("key", "");
    auto comboProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", comboKey}},
        &isError);
    ASSERT_FALSE(isError) << comboProperties.dump(2);
    const auto* selected =
        find_property_descriptor(comboProperties, "SelectedIndex");
    ASSERT_NE(selected, nullptr);
    EXPECT_TRUE(selected->value("supportsClear", false));
    auto cleared = client.call_tool(
        "clear_property",
        json{{"session", session},
             {"element", comboKey},
             {"descriptorId", selected->value("descriptorId", "")}},
        &isError);
    ASSERT_FALSE(isError) << cleared.dump(2);
    EXPECT_TRUE(cleared.value("cleared", false));
    schemaError.clear();
    EXPECT_TRUE(schema_allows(
        schemas.at("clear_property"), cleared, schemaError))
        << schemaError;
    auto restored = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", comboKey},
             {"descriptorId", selected->value("descriptorId", "")},
             {"value", "1"}},
        &isError);
    ASSERT_FALSE(isError) << restored.dump(2);

    ASSERT_GE(status->value("children", json::array()).size(), 2u);
    const auto firstPartKey =
        (*status)["children"][0].value("key", "");
    const auto secondPartKey =
        (*status)["children"][1].value("key", "");
    auto firstPart = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", firstPartKey}},
        &isError);
    ASSERT_FALSE(isError) << firstPart.dump(2);
    auto secondPart = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", secondPartKey}},
        &isError);
    ASSERT_FALSE(isError) << secondPart.dump(2);
    EXPECT_EQ(
        firstPart.value("schemaId", ""),
        secondPart.value("schemaId", ""))
        << "status parts should reuse one cached native schema";

    const auto parallelSession = connect(client);
    ASSERT_FALSE(parallelSession.empty());
    auto beforeParallelRefresh = client.call_tool(
        "get_editable_properties",
        json{{"session", parallelSession}, {"element", genericKey}},
        &isError);
    EXPECT_TRUE(isError) << beforeParallelRefresh.dump(2);
    auto parallelTree = client.call_tool(
        "get_visual_tree", json{{"session", parallelSession}},
        &isError);
    ASSERT_FALSE(isError) << parallelTree.dump(2);
    const auto* parallelGeneric = find_by_class(
        parallelTree["root"], "LvtNativePropertyFixtureText");
    ASSERT_NE(parallelGeneric, nullptr);
    auto parallelProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", parallelSession},
             {"element", parallelGeneric->value("key", "")}},
        &isError);
    EXPECT_FALSE(isError) << parallelProperties.dump(2);
    auto narrowedParallelTree = client.call_tool(
        "get_visual_tree",
        json{{"session", parallelSession}, {"element", genericKey}},
        &isError);
    ASSERT_FALSE(isError) << narrowedParallelTree.dump(2);
    auto retainedParallelSibling = client.call_tool(
        "get_editable_properties",
        json{{"session", parallelSession}, {"element", comboKey}},
        &isError);
    EXPECT_FALSE(isError) << retainedParallelSibling.dump(2);

    const auto scopedSession = connect(client);
    ASSERT_FALSE(scopedSession.empty());
    auto scopedTree = client.call_tool(
        "get_visual_tree",
        json{{"session", scopedSession}, {"element", genericKey}},
        &isError);
    ASSERT_FALSE(isError) << scopedTree.dump(2);
    auto scopedGenericProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", scopedSession}, {"element", genericKey}},
        &isError);
    EXPECT_FALSE(isError) << scopedGenericProperties.dump(2);
    auto underlyingSibling = client.call_tool(
        "get_editable_properties",
        json{{"session", scopedSession}, {"element", comboKey}},
        &isError);
    EXPECT_FALSE(isError) << underlyingSibling.dump(2);
}

TEST_F(NativeMcpFixture, NativeKeysRoundTripBetweenOneShotAndMcp) {
    const auto oneShotOutput = run_command(
        "\"" + get_lvt_path() + "\" --hwnd " + hwnd_string());
    const auto oneShot = json::parse(
        oneShotOutput, nullptr, false);
    ASSERT_FALSE(oneShot.is_discarded()) << oneShotOutput;
    ASSERT_TRUE(oneShot.contains("root"));
    const auto* oneShotGeneric = find_by_class(
        oneShot["root"], "LvtNativePropertyFixtureText");
    const auto* oneShotList = find_by_class(
        oneShot["root"], "SysListView32");
    ASSERT_NE(oneShotGeneric, nullptr);
    ASSERT_NE(oneShotList, nullptr);
    const auto* oneShotBeta = find_child(
        *oneShotList, "ListViewItem", "Beta row");
    ASSERT_NE(oneShotBeta, nullptr);

    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    bool isError = false;
    const auto persistent = client.call_tool(
        "get_visual_tree", json{{"session", session}},
        &isError);
    ASSERT_FALSE(isError) << persistent.dump(2);
    ASSERT_TRUE(persistent.contains("root"));
    const auto* persistentGeneric = find_by_class(
        persistent["root"], "LvtNativePropertyFixtureText");
    const auto* persistentList = find_by_class(
        persistent["root"], "SysListView32");
    ASSERT_NE(persistentGeneric, nullptr);
    ASSERT_NE(persistentList, nullptr);
    const auto* persistentBeta = find_child(
        *persistentList, "ListViewItem", "Beta row");
    ASSERT_NE(persistentBeta, nullptr);

    EXPECT_EQ(
        persistent["root"].value("key", ""),
        oneShot["root"].value("key", ""));
    EXPECT_EQ(
        persistentGeneric->value("key", ""),
        oneShotGeneric->value("key", ""));
    EXPECT_EQ(
        persistentBeta->value("key", ""),
        oneShotBeta->value("key", ""));

    const auto dumpKey = oneShotBeta->value("key", "");
    const auto scoped = client.call_tool(
        "get_visual_tree",
        json{{"session", session}, {"element", dumpKey}},
        &isError);
    ASSERT_FALSE(isError) << scoped.dump(2);
    ASSERT_TRUE(scoped.contains("root"));
    EXPECT_EQ(scoped["root"].value("key", ""), dumpKey);
    EXPECT_EQ(scoped["root"].value("text", ""), "Beta row");

    const auto mcpKey = persistentBeta->value("key", "");
    const auto queried = run_command(
        "\"" + get_lvt_path() + "\" --hwnd " + hwnd_string() +
        " query " + cmd_escape_arg(mcpKey) + " index");
    EXPECT_EQ(trim_crlf(queried), "1")
        << "a persistent MCP key must resolve in a one-shot query";
}

TEST_F(
    NativeMcpFixture,
    DurableIdentityChangeRemovesOldKeyAndAddsOneShotKey) {
    auto restore = wil::scope_exit([&] {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kRestoreListViewIdentityMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            2000, &ignored);
    });

    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    bool isError = false;
    auto initialTree = client.call_tool(
        "get_visual_tree", json{{"session", session}},
        &isError);
    ASSERT_FALSE(isError) << initialTree.dump(2);
    const auto* initialList = find_by_class(
        initialTree["root"], "SysListView32");
    ASSERT_NE(initialList, nullptr);
    const auto* alpha = find_child(
        *initialList, "ListViewItem", "Alpha row");
    ASSERT_NE(alpha, nullptr);
    const auto oldKey = alpha->value("key", "");
    ASSERT_FALSE(oldKey.empty());

    auto baseline = client.call_tool(
        "get_visual_tree_changes",
        json{{"session", session}}, &isError);
    ASSERT_FALSE(isError) << baseline.dump(2);
    ASSERT_TRUE(baseline.value("snapshot", false));

    DWORD_PTR changed = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd,
            native_fixture::kMutateListViewIdentityMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            2000, &changed),
        0);

    const auto oneShotOutput = run_command(
        "\"" + get_lvt_path() + "\" --hwnd " + hwnd_string());
    const auto oneShot = json::parse(
        oneShotOutput, nullptr, false);
    ASSERT_FALSE(oneShot.is_discarded()) << oneShotOutput;
    const auto* oneShotList = find_by_class(
        oneShot["root"], "SysListView32");
    ASSERT_NE(oneShotList, nullptr);
    const auto* replacement = find_child(
        *oneShotList, "ListViewItem", "External replacement");
    ASSERT_NE(replacement, nullptr);
    const auto freshKey = replacement->value("key", "");
    ASSERT_FALSE(freshKey.empty());
    ASSERT_NE(freshKey, oldKey);

    auto changes = client.call_tool(
        "get_visual_tree_changes",
        json{{"session", session}}, &isError);
    ASSERT_FALSE(isError) << changes.dump(2);
    ASSERT_FALSE(changes.value("snapshot", true))
        << changes.dump(2);
    bool removedOld = false;
    bool addedFresh = false;
    bool reusedOld = false;
    for (const auto& event :
         changes.value("events", json::array())) {
        const auto key = event.value("key", "");
        const auto type = event.value("event", "");
        removedOld =
            removedOld || (type == "removed" && key == oldKey);
        addedFresh =
            addedFresh || (type == "added" && key == freshKey);
        reusedOld =
            reusedOld || (type != "removed" && key == oldKey);
    }
    EXPECT_TRUE(removedOld) << changes.dump(2);
    EXPECT_TRUE(addedFresh) << changes.dump(2);
    EXPECT_FALSE(reusedOld) << changes.dump(2);

    auto scoped = client.call_tool(
        "get_visual_tree",
        json{{"session", session}, {"element", freshKey}},
        &isError);
    ASSERT_FALSE(isError) << scoped.dump(2);
    EXPECT_EQ(
        scoped["root"].value("text", ""),
        "External replacement");

    const auto queried = run_command(
        "\"" + get_lvt_path() + "\" --hwnd " + hwnd_string() +
        " query " + cmd_escape_arg(freshKey) + " index");
    EXPECT_EQ(trim_crlf(queried), "0");
}

TEST_F(
    NativeMcpFixture,
    OverlappingScopedTreeCannotInvalidateTargetsInLaterFullResponse) {
    NativePublicationGate gate("get_visual_tree");
    ASSERT_TRUE(gate.valid());
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());
    const auto genericKey = control_key(native_fixture::kGenericTextId);
    const auto comboKey = control_key(native_fixture::kComboBoxId);
    ASSERT_FALSE(genericKey.empty());
    ASSERT_FALSE(comboKey.empty());

    const int full = client.send_request(
        "tools/call",
        json{{"name", "get_visual_tree"},
             {"arguments", json{{"session", session}}}});
    ASSERT_TRUE(gate.wait_until_entered())
        << "the full response did not reach the post-publication gate";

    const int scoped = client.send_request(
        "tools/call",
        json{{"name", "get_visual_tree"},
             {"arguments",
              json{{"session", session}, {"element", genericKey}}}});
    const auto scopedResponse = client.await_response(scoped);
    ASSERT_TRUE(scopedResponse.contains("result"))
        << scopedResponse.dump(2);
    ASSERT_FALSE(scopedResponse["result"].value("isError", true))
        << scopedResponse.dump(2);

    gate.release();
    const auto fullResponse = client.await_response(full);
    ASSERT_TRUE(fullResponse.contains("result"))
        << fullResponse.dump(2);
    ASSERT_FALSE(fullResponse["result"].value("isError", true))
        << fullResponse.dump(2);
    const auto fullTree = json::parse(
        fullResponse["result"]["content"][0].value("text", "{}"),
        nullptr, false);
    ASSERT_FALSE(fullTree.is_discarded())
        << fullResponse.dump(2);
    ASSERT_NE(find_by_class(fullTree["root"], "ComboBox"), nullptr)
        << fullTree.dump(2);

    bool isError = false;
    const auto properties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", comboKey}},
        &isError);
    EXPECT_FALSE(isError)
        << "a key in the later-delivered full response was revoked by "
           "the overlapping scoped response: "
        << properties.dump(2);
}

TEST_F(
    NativeMcpFixture,
    OverlappingScopedTreeCannotInvalidateTargetsInLaterTreeChangeResponse) {
    NativePublicationGate gate("get_visual_tree_changes");
    ASSERT_TRUE(gate.valid());
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());
    const auto genericKey = control_key(native_fixture::kGenericTextId);
    const auto comboKey = control_key(native_fixture::kComboBoxId);
    ASSERT_FALSE(genericKey.empty());
    ASSERT_FALSE(comboKey.empty());

    const int changes = client.send_request(
        "tools/call",
        json{{"name", "get_visual_tree_changes"},
             {"arguments", json{{"session", session}}}});
    ASSERT_TRUE(gate.wait_until_entered())
        << "the tree-change response did not reach the publication gate";

    const int scoped = client.send_request(
        "tools/call",
        json{{"name", "get_visual_tree"},
             {"arguments",
              json{{"session", session}, {"element", genericKey}}}});
    const auto scopedResponse = client.await_response(scoped);
    ASSERT_TRUE(scopedResponse.contains("result"))
        << scopedResponse.dump(2);
    ASSERT_FALSE(scopedResponse["result"].value("isError", true))
        << scopedResponse.dump(2);

    gate.release();
    const auto changesResponse = client.await_response(changes);
    ASSERT_TRUE(changesResponse.contains("result"))
        << changesResponse.dump(2);
    ASSERT_FALSE(changesResponse["result"].value("isError", true))
        << changesResponse.dump(2);
    const auto patch = json::parse(
        changesResponse["result"]["content"][0].value("text", "{}"),
        nullptr, false);
    ASSERT_FALSE(patch.is_discarded())
        << changesResponse.dump(2);
    ASSERT_TRUE(patch.value("snapshot", false))
        << patch.dump(2);
    const auto events =
        patch.value("events", json::array());
    const bool returnedCombo = std::any_of(
        events.begin(), events.end(),
        [&comboKey](const json& event) {
            return event.value("key", "") == comboKey;
        });
    ASSERT_TRUE(returnedCombo) << patch.dump(2);

    bool isError = false;
    const auto properties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", comboKey}},
        &isError);
    EXPECT_FALSE(isError)
        << "a key in the later-delivered tree-change response was revoked "
           "by the overlapping scoped response: "
        << properties.dump(2);
}

TEST_F(
    NativeMcpFixture,
    OverlappingScopedTreeCannotInvalidateTargetsInLaterResourceSnapshot) {
    NativePublicationGate gate("get_visual_tree_changes");
    ASSERT_TRUE(gate.valid());
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());
    const auto genericKey = control_key(native_fixture::kGenericTextId);
    const auto comboKey = control_key(native_fixture::kComboBoxId);
    ASSERT_FALSE(genericKey.empty());
    ASSERT_FALSE(comboKey.empty());
    const auto uri =
        "lvt://session/" + session + "/visual-tree";

    const int resourceRead = client.send_request(
        "resources/read", json{{"uri", uri}});
    ASSERT_TRUE(gate.wait_until_entered())
        << "the resource snapshot did not reach the publication gate";

    const int scoped = client.send_request(
        "tools/call",
        json{{"name", "get_visual_tree"},
             {"arguments",
              json{{"session", session}, {"element", genericKey}}}});
    const auto scopedResponse = client.await_response(scoped);
    ASSERT_TRUE(scopedResponse.contains("result"))
        << scopedResponse.dump(2);
    ASSERT_FALSE(scopedResponse["result"].value("isError", true))
        << scopedResponse.dump(2);

    gate.release();
    const auto resourceResponse =
        client.await_response(resourceRead);
    ASSERT_TRUE(resourceResponse.contains("result"))
        << resourceResponse.dump(2);
    ASSERT_FALSE(
        resourceResponse["result"]
            .value("contents", json::array())
            .empty())
        << resourceResponse.dump(2);
    const auto patch = json::parse(
        resourceResponse["result"]["contents"][0]
            .value("text", "{}"),
        nullptr, false);
    ASSERT_FALSE(patch.is_discarded())
        << resourceResponse.dump(2);
    const auto events =
        patch.value("events", json::array());
    const bool returnedCombo = std::any_of(
        events.begin(), events.end(),
        [&comboKey](const json& event) {
            return event.value("key", "") == comboKey;
        });
    ASSERT_TRUE(returnedCombo) << patch.dump(2);

    bool isError = false;
    const auto properties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", comboKey}},
        &isError);
    EXPECT_FALSE(isError)
        << "a key in the later-delivered resource snapshot was revoked by "
           "the overlapping scoped response: "
        << properties.dump(2);
}

TEST_F(
    NativeMcpFixture,
    CompleteSnapshotInvalidatesTargetReparentedOutOfSessionRoot) {
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());
    const auto genericKey = control_key(native_fixture::kGenericTextId);
    ASSERT_FALSE(genericKey.empty());

    bool isError = false;
    const auto initial = client.call_tool(
        "get_visual_tree", json{{"session", session}},
        &isError);
    ASSERT_FALSE(isError) << initial.dump(2);
    const auto baseline = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", genericKey}},
        &isError);
    ASSERT_FALSE(isError) << baseline.dump(2);

    DWORD_PTR oldParent = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd,
            native_fixture::kReparentGenericOutOfTreeMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            2000, &oldParent),
        0);
    auto restore = wil::scope_exit([&] {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(
            s_hwnd,
            native_fixture::kRestoreGenericParentMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            2000, &ignored);
    });

    const auto promptlyRejected = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", genericKey}},
        &isError);
    EXPECT_TRUE(isError)
        << "a target that moved out of the session root remained usable "
           "until the next snapshot: "
        << promptlyRejected.dump(2);

    const auto withoutGeneric = client.call_tool(
        "get_visual_tree", json{{"session", session}},
        &isError);
    ASSERT_FALSE(isError) << withoutGeneric.dump(2);
    EXPECT_EQ(
        find_by_class(
            withoutGeneric["root"],
            "LvtNativePropertyFixtureText"),
        nullptr)
        << withoutGeneric.dump(2);
    const auto rejected = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", genericKey}},
        &isError);
    EXPECT_TRUE(isError)
        << "a complete root snapshot retained a target that moved out "
           "of the session root: "
        << rejected.dump(2);

    DWORD_PTR ignored = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd,
            native_fixture::kRestoreGenericParentMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            2000, &ignored),
        0);
    restore.release();
    const auto restoredTree = client.call_tool(
        "get_visual_tree", json{{"session", session}},
        &isError);
    ASSERT_FALSE(isError) << restoredTree.dump(2);
    const auto restoredProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", genericKey}},
        &isError);
    EXPECT_FALSE(isError) << restoredProperties.dump(2);
}

TEST_F(
    NativeMcpFixture,
    DisconnectClearsTargetsWhilePublishedResponseAwaitsDelivery) {
    NativePublicationGate gate("get_visual_tree");
    ASSERT_TRUE(gate.valid());
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());
    const auto genericKey = control_key(native_fixture::kGenericTextId);
    ASSERT_FALSE(genericKey.empty());

    const int tree = client.send_request(
        "tools/call",
        json{{"name", "get_visual_tree"},
             {"arguments", json{{"session", session}}}});
    ASSERT_TRUE(gate.wait_until_entered())
        << "the tree response did not reach the publication gate";

    const int disconnect = client.send_request(
        "tools/call",
        json{{"name", "disconnect"},
             {"arguments", json{{"session", session}}}});
    const auto disconnectResponse =
        client.await_response(disconnect);
    ASSERT_TRUE(disconnectResponse.contains("result"))
        << disconnectResponse.dump(2);
    ASSERT_FALSE(
        disconnectResponse["result"].value("isError", true))
        << disconnectResponse.dump(2);

    gate.release();
    const auto treeResponse = client.await_response(tree);
    ASSERT_TRUE(treeResponse.contains("result"))
        << treeResponse.dump(2);
    ASSERT_FALSE(treeResponse["result"].value("isError", true))
        << treeResponse.dump(2);

    bool isError = false;
    const auto disconnected = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", genericKey}},
        &isError);
    EXPECT_TRUE(isError) << disconnected.dump(2);

    const auto replacement = connect(client);
    ASSERT_FALSE(replacement.empty());
    const auto unpublished = client.call_tool(
        "get_editable_properties",
        json{{"session", replacement}, {"element", genericKey}},
        &isError);
    EXPECT_TRUE(isError)
        << "disconnect leaked published targets into a replacement session: "
        << unpublished.dump(2);
}

TEST_F(NativeMcpFixture, DisconnectRacingNativePropertyReadDoesNotRecreateSession) {
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto tree = client.call_tool(
        "get_visual_tree", json{{"session", session}});
    ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
    const auto* generic =
        find_by_class(tree["root"], "LvtNativePropertyFixtureText");
    const auto* toolbar =
        find_by_class(tree["root"], "ToolbarWindow32");
    ASSERT_NE(generic, nullptr);
    ASSERT_NE(toolbar, nullptr);
    const auto* apply =
        find_child(*toolbar, "ToolbarButton", "Apply");
    ASSERT_NE(apply, nullptr);
    const auto genericKey = generic->value("key", "");
    const auto applyKey = apply->value("key", "");

    bool isError = false;
    auto baseline = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", applyKey}},
        &isError);
    ASSERT_FALSE(isError) << baseline.dump(2);

    DWORD_PTR armed = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kArmDelayedPointerMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000, &armed),
        0);

    const int slow = client.send_request(
        "tools/call",
        json{{"name", "get_editable_properties"},
             {"arguments",
              json{{"session", session}, {"element", applyKey}}}});
    Sleep(100);
    const int queued = client.send_request(
        "tools/call",
        json{{"name", "get_editable_properties"},
             {"arguments",
              json{{"session", session}, {"element", genericKey}}}});
    const int disconnecting = client.send_request(
        "tools/call",
        json{{"name", "disconnect"},
             {"arguments", json{{"session", session}}}});

    auto slowResponse = client.await_response(slow);
    auto queuedResponse = client.await_response(queued);
    auto disconnected = client.await_response(disconnecting);
    ASSERT_TRUE(slowResponse.contains("result"));
    ASSERT_TRUE(queuedResponse.contains("result"));
    ASSERT_TRUE(disconnected.contains("result"));
    EXPECT_TRUE(slowResponse["result"].value("isError", false));
    EXPECT_TRUE(queuedResponse["result"].value("isError", false))
        << queuedResponse.dump(2);
    EXPECT_FALSE(disconnected["result"].value("isError", true))
        << disconnected.dump(2);

    auto oldSession = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", genericKey}},
        &isError);
    EXPECT_TRUE(isError) << oldSession.dump(2);
    expect_typed_property_session_disconnected(oldSession);

    const auto replacement = connect(client);
    ASSERT_FALSE(replacement.empty());
    auto beforeRefresh = client.call_tool(
        "get_editable_properties",
        json{{"session", replacement}, {"element", genericKey}},
        &isError);
    EXPECT_TRUE(isError) << beforeRefresh.dump(2);
    EXPECT_NE(
        beforeRefresh.value("error", "").find("unknown"),
        std::string::npos);

    auto replacementTree = client.call_tool(
        "get_visual_tree", json{{"session", replacement}},
        &isError);
    ASSERT_FALSE(isError) << replacementTree.dump(2);
    const auto* replacementGeneric = find_by_class(
        replacementTree["root"], "LvtNativePropertyFixtureText");
    ASSERT_NE(replacementGeneric, nullptr);
    auto replacementProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", replacement},
             {"element", replacementGeneric->value("key", "")}},
        &isError);
    EXPECT_FALSE(isError) << replacementProperties.dump(2);
}

TEST_F(McpSampleFixture, TypedPropertyConnectionErrorsHaveDisposition) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client, "visual");
    ASSERT_FALSE(session.empty());

    bool isError = false;
    auto temporarilyUnavailable = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", "xaml:0x1"}},
        &isError);
    ASSERT_TRUE(isError) << temporarilyUnavailable.dump(2);
    EXPECT_EQ(
        temporarilyUnavailable.value("errorCode", ""),
        "typed_property_connection_unavailable");
    EXPECT_EQ(
        temporarilyUnavailable.value("errorDisposition", ""),
        "transient");
    EXPECT_TRUE(temporarilyUnavailable.value("retryable", false));

    auto unsupported = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", "unsupported_plugin:0x1"}},
        &isError);
    ASSERT_TRUE(isError) << unsupported.dump(2);
    EXPECT_EQ(
        unsupported.value("errorCode", ""),
        "typed_property_provider_unsupported");
    EXPECT_EQ(unsupported.value("errorDisposition", ""), "terminal");
    EXPECT_FALSE(unsupported.value("retryable", true));

    auto visual = client.call_tool(
        "get_visual_tree", json{{"session", session}}, &isError);
    ASSERT_FALSE(isError) << visual.dump(2);
    std::vector<const json*> elements;
    collect_json_elements(visual["root"], elements);
    const json* button = nullptr;
    for (const auto* element : elements) {
        if (element->value("properties", json::object())
                .value("name", "") == "PrimaryButton") {
            button = element;
            break;
        }
    }
    ASSERT_NE(button, nullptr);
    auto recovered = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", button->value("key", "")}},
        &isError);
    EXPECT_FALSE(isError) << recovered.dump(2);
    EXPECT_FALSE(recovered.value("schemaId", "").empty())
        << recovered.dump(2);
}

TEST_F(McpSampleFixture, ResourcesMatchEachSessionsFixedTreeMode) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    json serverInfo;
    ASSERT_TRUE(client.handshake(&serverInfo));
    ASSERT_TRUE(serverInfo["capabilities"].contains("resources"))
        << serverInfo.dump(2);
    EXPECT_TRUE(serverInfo["capabilities"]["resources"].value("subscribe", false));

    const auto uiaSession = connect(client);
    const auto visualSession = connect(client, "visual");
    ASSERT_FALSE(uiaSession.empty());
    ASSERT_FALSE(visualSession.empty());

    auto listed = client.request("resources/list");
    ASSERT_TRUE(listed.contains("result")) << listed.dump(2);
    std::vector<std::string> uris;
    for (const auto& resource : listed["result"].value("resources", json::array()))
        uris.push_back(resource.value("uri", ""));

    const auto uiaUri = "lvt://session/" + uiaSession + "/uia-tree";
    const auto wrongUiaUri = "lvt://session/" + uiaSession + "/visual-tree";
    const auto visualUri = "lvt://session/" + visualSession + "/visual-tree";
    const auto wrongVisualUri = "lvt://session/" + visualSession + "/uia-tree";
    EXPECT_NE(std::find(uris.begin(), uris.end(), uiaUri), uris.end());
    EXPECT_NE(std::find(uris.begin(), uris.end(), visualUri), uris.end());
    EXPECT_EQ(std::find(uris.begin(), uris.end(), wrongUiaUri), uris.end());
    EXPECT_EQ(std::find(uris.begin(), uris.end(), wrongVisualUri), uris.end());
}

TEST_F(McpSampleFixture, UiaTypedPropertiesStayScopedToEachWindowInOneProcess) {
    SkipIfNotReady();
    const auto secondaryHwnd = secondary_hwnd_string();
    if (secondaryHwnd.empty())
        GTEST_SKIP() << "the sample app's secondary window is unavailable";

    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto primarySession = connect(client);
    ASSERT_FALSE(primarySession.empty());
    const auto secondaryConnect = client.call_tool(
        "connect", json{{"hwnd", secondaryHwnd}, {"mode", "uia"}});
    const auto secondarySession = secondaryConnect.value("session", "");
    ASSERT_FALSE(secondarySession.empty()) << secondaryConnect.dump(2);

    auto primaryInput = client.call_tool(
        "find_elements",
        json{{"session", primarySession}, {"automationId", "InputBox"}});
    auto secondaryInput = client.call_tool(
        "find_elements",
        json{{"session", secondarySession}, {"automationId", "2001"}});
    ASSERT_EQ(primaryInput["elements"].size(), 1u) << primaryInput.dump(2);
    ASSERT_EQ(secondaryInput["elements"].size(), 1u) << secondaryInput.dump(2);
    EXPECT_TRUE(client.call_tool(
        "find_elements",
        json{{"session", primarySession}, {"automationId", "2001"}})
                    ["elements"].empty());
    EXPECT_TRUE(client.call_tool(
        "find_elements",
        json{{"session", secondarySession}, {"automationId", "InputBox"}})
                    ["elements"].empty());

    const auto primaryKey = primaryInput["elements"][0].value("key", "");
    const auto secondaryKey = secondaryInput["elements"][0].value("key", "");
    bool isError = false;
    auto primaryProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", primarySession}, {"element", primaryKey}}, &isError);
    ASSERT_FALSE(isError) << primaryProperties.dump(2);
    auto secondaryProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", secondarySession}, {"element", secondaryKey}}, &isError);
    ASSERT_FALSE(isError) << secondaryProperties.dump(2);
    const auto* primaryValue =
        find_property_descriptor(primaryProperties, "Value.Value");
    const auto* secondaryValue =
        find_property_descriptor(secondaryProperties, "Value.Value");
    ASSERT_NE(primaryValue, nullptr);
    ASSERT_NE(secondaryValue, nullptr);
    const auto originalPrimary =
        typed_property_value(primaryProperties, "Value.Value");
    const auto originalSecondary =
        typed_property_value(secondaryProperties, "Value.Value");

    auto setPrimary = client.call_tool(
        "set_property",
        json{{"session", primarySession},
             {"element", primaryKey},
             {"descriptorId", primaryValue->value("descriptorId", "")},
             {"value", "primary isolated"}},
        &isError);
    ASSERT_FALSE(isError) << setPrimary.dump(2);
    auto setSecondary = client.call_tool(
        "set_property",
        json{{"session", secondarySession},
             {"element", secondaryKey},
             {"descriptorId", secondaryValue->value("descriptorId", "")},
             {"value", "secondary isolated"}},
        &isError);
    ASSERT_FALSE(isError) << setSecondary.dump(2);

    auto primaryAfter = client.call_tool(
        "get_editable_properties",
        json{{"session", primarySession}, {"element", primaryKey}}, &isError);
    ASSERT_FALSE(isError) << primaryAfter.dump(2);
    auto secondaryAfter = client.call_tool(
        "get_editable_properties",
        json{{"session", secondarySession}, {"element", secondaryKey}}, &isError);
    ASSERT_FALSE(isError) << secondaryAfter.dump(2);
    EXPECT_EQ(typed_property_value(primaryAfter, "Value.Value"), "primary isolated");
    EXPECT_EQ(typed_property_value(secondaryAfter, "Value.Value"), "secondary isolated");

    auto primaryCheck = client.call_tool(
        "find_elements",
        json{{"session", primarySession}, {"automationId", "ReadyCheckBox"}});
    auto secondaryCheck = client.call_tool(
        "find_elements",
        json{{"session", secondarySession}, {"automationId", "2002"}});
    ASSERT_EQ(primaryCheck["elements"].size(), 1u);
    ASSERT_EQ(secondaryCheck["elements"].size(), 1u);
    const auto primaryCheckKey = primaryCheck["elements"][0].value("key", "");
    const auto secondaryCheckKey = secondaryCheck["elements"][0].value("key", "");
    auto primaryCheckProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", primarySession}, {"element", primaryCheckKey}}, &isError);
    auto secondaryCheckProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", secondarySession}, {"element", secondaryCheckKey}}, &isError);
    ASSERT_FALSE(isError);
    const auto* primaryToggle =
        find_property_descriptor(primaryCheckProperties, "Toggle.ToggleState");
    const auto* secondaryToggle =
        find_property_descriptor(secondaryCheckProperties, "Toggle.ToggleState");
    ASSERT_NE(primaryToggle, nullptr);
    ASSERT_NE(secondaryToggle, nullptr);
    const auto originalPrimaryToggle =
        typed_property_value(primaryCheckProperties, "Toggle.ToggleState");
    const auto originalSecondaryToggle =
        typed_property_value(secondaryCheckProperties, "Toggle.ToggleState");

    auto primaryOff = client.call_tool(
        "set_property",
        json{{"session", primarySession},
             {"element", primaryCheckKey},
             {"descriptorId", primaryToggle->value("descriptorId", "")},
             {"value", "Off"}},
        &isError);
    ASSERT_FALSE(isError) << primaryOff.dump(2);
    auto secondaryOn = client.call_tool(
        "set_property",
        json{{"session", secondarySession},
             {"element", secondaryCheckKey},
             {"descriptorId", secondaryToggle->value("descriptorId", "")},
             {"value", "On"}},
        &isError);
    ASSERT_FALSE(isError) << secondaryOn.dump(2);

    auto primaryToggleAfter = client.call_tool(
        "get_editable_properties",
        json{{"session", primarySession}, {"element", primaryCheckKey}}, &isError);
    auto secondaryToggleAfter = client.call_tool(
        "get_editable_properties",
        json{{"session", secondarySession}, {"element", secondaryCheckKey}}, &isError);
    ASSERT_FALSE(isError);
    EXPECT_EQ(
        typed_property_value(primaryToggleAfter, "Toggle.ToggleState"), "Off");
    EXPECT_EQ(
        typed_property_value(secondaryToggleAfter, "Toggle.ToggleState"), "On");

    client.call_tool(
        "set_property",
        json{{"session", primarySession},
             {"element", primaryKey},
             {"descriptorId", primaryValue->value("descriptorId", "")},
             {"value", originalPrimary}},
        &isError);
    client.call_tool(
        "set_property",
        json{{"session", secondarySession},
             {"element", secondaryKey},
             {"descriptorId", secondaryValue->value("descriptorId", "")},
             {"value", originalSecondary}},
        &isError);
    client.call_tool(
        "set_property",
        json{{"session", primarySession},
             {"element", primaryCheckKey},
             {"descriptorId", primaryToggle->value("descriptorId", "")},
             {"value", originalPrimaryToggle}},
        &isError);
    client.call_tool(
        "set_property",
        json{{"session", secondarySession},
             {"element", secondaryCheckKey},
             {"descriptorId", secondaryToggle->value("descriptorId", "")},
             {"value", originalSecondaryToggle}},
        &isError);
    EXPECT_FALSE(isError);
}

TEST_F(McpSampleFixture, UiaTypedPropertiesPreserveOriginatingViewIdentity) {
    SkipIfNotReady();

    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto raw = client.call_tool(
        "find_elements",
        json{{"session", session},
             {"automationId", "RawOnlyInput"},
             {"view", "raw"}});
    ASSERT_EQ(raw["elements"].size(), 1u) << raw.dump(2);
    const auto rawRef = raw["elements"][0].value("ref", "");
    const auto rawKey = raw["elements"][0].value("key", "");

    bool isError = false;
    auto rawProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", rawKey}}, &isError);
    ASSERT_FALSE(isError) << rawProperties.dump(2);
    EXPECT_NE(
        find_property_descriptor(rawProperties, "Value.Value"), nullptr);

    auto rawElement = client.call_tool(
        "get_element_properties",
        json{{"session", session},
             {"element", rawRef},
             {"view", "raw"},
             {"properties", json::array({"RuntimeId"})}},
        &isError);
    ASSERT_FALSE(isError) << rawElement.dump(2);
    const auto runtimeId =
        rawElement["properties"].value("RuntimeId", "");
    ASSERT_FALSE(runtimeId.empty()) << rawElement.dump(2);
    auto directRuntime = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", "uia:" + runtimeId}},
        &isError);
    ASSERT_FALSE(isError) << directRuntime.dump(2);
    EXPECT_EQ(
        directRuntime.value("schemaId", ""),
        rawProperties.value("schemaId", ""));

    auto positional = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", rawRef}}, &isError);
    EXPECT_TRUE(isError) << positional.dump(2);
    EXPECT_NE(positional.dump().find("originating raw/control/content view"),
              std::string::npos) << positional.dump(2);

    auto content = client.call_tool(
        "find_elements",
        json{{"session", session},
             {"automationId", "InputBox"},
             {"view", "content"}});
    ASSERT_EQ(content["elements"].size(), 1u) << content.dump(2);
    auto contentProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session},
             {"element", content["elements"][0].value("key", "")}},
        &isError);
    ASSERT_FALSE(isError) << contentProperties.dump(2);
    EXPECT_NE(
        find_property_descriptor(contentProperties, "Value.Value"), nullptr);

    auto rawAfterOtherViews = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", rawKey}}, &isError);
    ASSERT_FALSE(isError) << rawAfterOtherViews.dump(2);
    EXPECT_EQ(
        rawAfterOtherViews.value("schemaId", ""),
        rawProperties.value("schemaId", ""));
}

TEST_F(McpSampleFixture, UiaIdentityScopeIgnoresTimeoutAndPropertyChurn) {
    SkipIfNotReady();

    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto raw = client.call_tool(
        "find_elements",
        json{{"session", session},
             {"automationId", "RawOnlyInput"},
             {"view", "raw"}});
    ASSERT_EQ(raw["elements"].size(), 1u) << raw.dump(2);
    const auto rawKey = raw["elements"][0].value("key", "");
    ASSERT_FALSE(rawKey.empty());

    const std::array<const char*, 4> properties{
        "Name", "AutomationId", "HelpText", "RuntimeId"};
    bool isError = false;
    for (int i = 0; i < 24; ++i) {
        auto requested = json::array(
            {properties[static_cast<size_t>(i) % properties.size()]});
        if (i % 3 == 0)
            requested.push_back("RuntimeId");
        auto tree = client.call_tool(
            "get_uia_tree",
            json{{"session", session},
                 {"view", "raw"},
                 {"depth", 0},
                 {"timeoutMs", 10000 + i},
                 {"properties", std::move(requested)}},
            &isError);
        ASSERT_FALSE(isError)
            << "UIA identity scope was exhausted by non-identity option "
               "combinations at iteration "
            << i << ": " << tree.dump(2);
        ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
    }

    for (const auto* view : {"control", "content"}) {
        auto tree = client.call_tool(
            "get_uia_tree",
            json{{"session", session},
                 {"view", view},
                 {"depth", 0},
                 {"timeoutMs", 10100},
                 {"properties", json::array({"Name"})}},
            &isError);
        ASSERT_FALSE(isError) << view << ": " << tree.dump(2);
        ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
    }

    auto rawProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", rawKey}},
        &isError);
    ASSERT_FALSE(isError) << rawProperties.dump(2);
    EXPECT_NE(
        find_property_descriptor(rawProperties, "Value.Value"), nullptr);
}

TEST_F(McpSampleFixture, TypedScrollRejectsAStaleMissingScrollPattern) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto list = client.call_tool(
        "find_elements",
        json{{"session", session}, {"automationId", "ItemsList"}});
    auto toggleSize = client.call_tool(
        "find_elements",
        json{{"session", session}, {"automationId", "ToggleListSizeButton"}});
    ASSERT_EQ(list["elements"].size(), 1u) << list.dump(2);
    ASSERT_EQ(toggleSize["elements"].size(), 1u) << toggleSize.dump(2);
    const auto listKey = list["elements"][0].value("key", "");
    const auto toggleKey = toggleSize["elements"][0].value("key", "");

    bool isError = false;
    auto before = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", listKey}}, &isError);
    ASSERT_FALSE(isError) << before.dump(2);
    const auto* scroll =
        find_property_descriptor(before, "Scroll.Action");
    ASSERT_NE(scroll, nullptr) << before.dump(2);
    ASSERT_TRUE(scroll->value("writable", false));
    const auto staleDescriptor = scroll->value("descriptorId", "");

    auto shrink = client.call_tool(
        "click",
        json{{"session", session}, {"element", toggleKey}}, &isError);
    ASSERT_FALSE(isError) << shrink.dump(2);

    auto changed = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", listKey}}, &isError);
    ASSERT_FALSE(isError) << changed.dump(2);
    const auto* changedScroll =
        find_property_descriptor(changed, "Scroll.Action");
    if (changedScroll) {
        EXPECT_FALSE(changedScroll->value("writable", true));
        EXPECT_NE(changedScroll->value("descriptorId", ""), staleDescriptor);
    }

    auto stale = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", listKey},
             {"descriptorId", staleDescriptor},
             {"value", "down"}},
        &isError);
    EXPECT_TRUE(isError) << stale.dump(2);
    EXPECT_NE(stale.dump().find("stale"), std::string::npos)
        << stale.dump(2);
    EXPECT_EQ(stale.dump().find("ScrollItemPattern"), std::string::npos)
        << "typed directional scroll must not fall back to ScrollIntoView";

    auto restore = client.call_tool(
        "click",
        json{{"session", session}, {"element", toggleKey}}, &isError);
    EXPECT_FALSE(isError) << restore.dump(2);
}

TEST_F(McpSampleFixture, RangeValueMutationReturnsProviderReadback) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto slider = client.call_tool(
        "find_elements",
        json{{"session", session}, {"automationId", "LevelSlider"}});
    ASSERT_EQ(slider["elements"].size(), 1u) << slider.dump(2);
    const auto key = slider["elements"][0].value("key", "");
    bool isError = false;
    auto before = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", key}}, &isError);
    ASSERT_FALSE(isError) << before.dump(2);
    const auto* range =
        find_property_descriptor(before, "RangeValue.Value");
    ASSERT_NE(range, nullptr) << before.dump(2);
    const auto descriptorId = range->value("descriptorId", "");
    const auto original = typed_property_value(before, "RangeValue.Value");

    const auto requested = "075.1300";
    auto mutation = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", key},
             {"descriptorId", descriptorId},
             {"value", requested}},
        &isError);
    ASSERT_FALSE(isError) << mutation.dump(2);
    const auto readback = mutation.value("value", "");
    ASSERT_FALSE(readback.empty()) << mutation.dump(2);
    EXPECT_NE(readback, requested);

    auto after = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", key}}, &isError);
    ASSERT_FALSE(isError) << after.dump(2);
    EXPECT_EQ(typed_property_value(after, "RangeValue.Value"), readback);

    auto restore = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", key},
             {"descriptorId", descriptorId},
             {"value", original}},
        &isError);
    EXPECT_FALSE(isError) << restore.dump(2);
}

TEST_F(McpSampleFixture, ResourceSubscriptionPushesVisualPropertyChange) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    const auto session = connect(client, "visual");
    ASSERT_FALSE(session.empty());
    auto visual = client.call_tool("get_visual_tree", json{{"session", session}});
    ASSERT_TRUE(visual.contains("root")) << visual.dump(2);
    std::vector<const json*> elements;
    collect_json_elements(visual["root"], elements);
    const json* status = nullptr;
    for (const auto* element : elements) {
        if (element->value("properties", json::object()).value("name", "") == "StatusText") {
            status = element;
            break;
        }
    }
    ASSERT_NE(status, nullptr);
    const auto statusKey = status->value("key", "");
    ASSERT_EQ(statusKey.rfind("winui3:0x", 0), 0u);

    bool isError = false;
    auto statusProperties = client.call_tool(
        "get_editable_properties",
        json{{"session", session}, {"element", statusKey}}, &isError);
    ASSERT_FALSE(isError) << statusProperties.dump(2);
    const auto* textProperty =
        find_property_descriptor(statusProperties, "Text");
    ASSERT_NE(textProperty, nullptr) << statusProperties.dump(2);
    const auto textDescriptorId =
        textProperty->value("descriptorId", "");
    const auto originalText = typed_property_value(statusProperties, "Text");

    auto listed = client.request("resources/list");
    ASSERT_TRUE(listed.contains("result")) << listed.dump(2);
    const auto uri = "lvt://session/" + session + "/visual-tree";
    bool foundResource = false;
    for (const auto& resource : listed["result"].value("resources", json::array()))
        foundResource = foundResource || resource.value("uri", "") == uri;
    ASSERT_TRUE(foundResource) << listed.dump(2);

    auto subscribed = client.request("resources/subscribe", json{{"uri", uri}});
    ASSERT_TRUE(subscribed.contains("result")) << subscribed.dump(2);

    auto initialRead = client.request("resources/read", json{{"uri", uri}});
    ASSERT_TRUE(initialRead.contains("result")) << initialRead.dump(2);
    ASSERT_FALSE(initialRead["result"].value("contents", json::array()).empty());
    const auto initialPatch = json::parse(
        initialRead["result"]["contents"][0].value("text", ""), nullptr, false);
    ASSERT_FALSE(initialPatch.is_discarded()) << initialRead.dump(2);
    EXPECT_TRUE(initialPatch.value("snapshot", false));

    // Direct tools and subscribed resources are independent consumers. Give
    // the direct tool its own baseline before the mutation; consuming its
    // later diff must not advance or steal the resource stream's baseline.
    auto directInitial = client.call_tool(
        "get_visual_tree_changes",
        json{{"session", session}, {"fast", true}}, &isError);
    ASSERT_FALSE(isError) << directInitial.dump(2);
    EXPECT_TRUE(directInitial.value("snapshot", false));

    const auto changedText = "resource visual mutation";
    auto changed = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", textDescriptorId},
             {"value", changedText}},
        &isError);
    ASSERT_FALSE(isError) << changed.dump(2);

    auto directChanged = client.call_tool(
        "get_visual_tree_changes",
        json{{"session", session}, {"fast", true}}, &isError);
    ASSERT_FALSE(isError) << directChanged.dump(2);
    EXPECT_FALSE(directChanged.value("snapshot", true));

    // No request is issued between the action and this receive. The message
    // must originate at the server after its periodic diff observes a
    // non-structural property/text mutation.
    auto notification =
        client.await_notification("notifications/resources/updated", 20);
    ASSERT_FALSE(notification.is_null())
        << "no unsolicited resource update arrived after the visual tree changed";
    EXPECT_EQ(notification["params"].value("uri", ""), uri);

    auto read = client.request("resources/read", json{{"uri", uri}});
    ASSERT_TRUE(read.contains("result")) << read.dump(2);
    ASSERT_FALSE(read["result"].value("contents", json::array()).empty());
    const auto patch = json::parse(
        read["result"]["contents"][0].value("text", ""), nullptr, false);
    ASSERT_FALSE(patch.is_discarded()) << read.dump(2);
    EXPECT_EQ(patch.value("tree", ""), "visual");
    EXPECT_FALSE(patch.value("snapshot", true));
    EXPECT_FALSE(patch.value("events", json::array()).empty());
    bool sawTextChange = false;
    for (const auto& event : patch.value("events", json::array())) {
        const auto fields = event.value("fields", json::object());
        sawTextChange = sawTextChange ||
                        (event.value("key", "") == statusKey &&
                         (fields.contains("text") || fields.contains("properties.Text")));
    }
    EXPECT_TRUE(sawTextChange) << patch.dump(2);

    auto unsubscribed = client.request("resources/unsubscribe", json{{"uri", uri}});
    EXPECT_TRUE(unsubscribed.contains("result")) << unsubscribed.dump(2);
    auto restored = client.call_tool(
        "set_property",
        json{{"session", session},
             {"element", statusKey},
             {"descriptorId", textDescriptorId},
             {"value", originalText}},
        &isError);
    EXPECT_FALSE(isError) << restored.dump(2);
}

TEST_F(McpSampleFixture, ResourceSubscriptionPushesUiaValueChange) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    const auto session = connect(client);
    ASSERT_FALSE(session.empty());
    auto input = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "InputBox"}});
    ASSERT_EQ(input["elements"].size(), 1u) << input.dump(2);
    const auto inputKey = input["elements"][0].value("key", "");
    auto before = client.call_tool(
        "get_element_properties",
        json{{"session", session},
             {"element", inputKey},
             {"properties", json::array({"Value.Value"})}});
    const auto originalValue = before["properties"].value("Value.Value", "");

    const auto uri = "lvt://session/" + session + "/uia-tree";
    auto subscribed = client.request("resources/subscribe", json{{"uri", uri}});
    ASSERT_TRUE(subscribed.contains("result")) << subscribed.dump(2);

    bool isError = false;
    const auto changedValue = "resource UIA mutation";
    auto changed = client.call_tool(
        "set_value",
        json{{"session", session}, {"element", inputKey}, {"text", changedValue}},
        &isError);
    ASSERT_FALSE(isError) << changed.dump(2);

    // No client request occurs between the mutation response and this await.
    // A passing test therefore proves standards-compliant server push rather
    // than a hidden resources/read polling loop in the client.
    auto notification =
        client.await_notification("notifications/resources/updated", 20);
    ASSERT_FALSE(notification.is_null())
        << "no unsolicited UIA resource update arrived after Value.Value changed";
    EXPECT_EQ(notification["params"].value("uri", ""), uri);

    auto read = client.request("resources/read", json{{"uri", uri}});
    ASSERT_TRUE(read.contains("result")) << read.dump(2);
    ASSERT_FALSE(read["result"].value("contents", json::array()).empty());
    const auto patch = json::parse(
        read["result"]["contents"][0].value("text", ""), nullptr, false);
    ASSERT_FALSE(patch.is_discarded()) << read.dump(2);
    EXPECT_EQ(patch.value("tree", ""), "uia");
    EXPECT_TRUE(patch.value("snapshot", false));
    bool sawValueChange = false;
    for (const auto& event : patch.value("events", json::array())) {
        const auto fields = event.value("fields", json::object());
        sawValueChange = sawValueChange ||
                         (event.value("key", "") == inputKey &&
                          (fields.contains("text") ||
                           fields.contains("properties.Value.Value")));
    }
    EXPECT_TRUE(sawValueChange) << patch.dump(2);

    auto unsubscribed = client.request("resources/unsubscribe", json{{"uri", uri}});
    EXPECT_TRUE(unsubscribed.contains("result")) << unsubscribed.dump(2);
    auto restored = client.call_tool(
        "set_value",
        json{{"session", session}, {"element", inputKey}, {"text", originalValue}},
        &isError);
    EXPECT_FALSE(isError) << restored.dump(2);
}

TEST_F(McpSampleFixture, ModernListenPushesAndCancelsUiaResourceUpdates) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake(nullptr, "2026-07-28"));

    const auto session = connect(client);
    ASSERT_FALSE(session.empty());
    auto checkbox = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "ReadyCheckBox"}});
    ASSERT_EQ(checkbox["elements"].size(), 1u) << checkbox.dump(2);
    const auto checkboxRef = checkbox["elements"][0].value("ref", "");
    const auto checkboxKey = checkbox["elements"][0].value("key", "");
    const auto uri = "lvt://session/" + session + "/uia-tree";

    const int listenId = client.send_request(
        "subscriptions/listen",
        json{{"notifications",
              json{{"resourceSubscriptions", json::array({uri})}}}});
    auto acknowledged =
        client.await_notification("notifications/subscriptions/acknowledged", 20);
    ASSERT_FALSE(acknowledged.is_null()) << "modern listen was not acknowledged";

    // Modern rmcp acknowledges before entering ServerHandler::listen. The
    // server therefore publishes the cached initial snapshot once it is ready.
    auto initialNotification =
        client.await_notification("notifications/resources/updated", 20);
    ASSERT_FALSE(initialNotification.is_null());
    auto initialRead = client.request("resources/read", json{{"uri", uri}});
    ASSERT_TRUE(initialRead.contains("result")) << initialRead.dump(2);
    const auto initialPatch = json::parse(
        initialRead["result"]["contents"][0].value("text", ""), nullptr, false);
    ASSERT_TRUE(initialPatch.value("snapshot", false)) << initialPatch.dump(2);

    bool isError = false;
    auto toggled = client.call_tool(
        "toggle", json{{"session", session}, {"element", checkboxRef}}, &isError);
    ASSERT_FALSE(isError) << toggled.dump(2);

    // No request is sent between the mutation and this receive.
    auto changedNotification =
        client.await_notification("notifications/resources/updated", 20);
    ASSERT_FALSE(changedNotification.is_null())
        << "modern subscription did not push the UIA change";
    auto changedRead = client.request("resources/read", json{{"uri", uri}});
    ASSERT_TRUE(changedRead.contains("result")) << changedRead.dump(2);
    const auto patch = json::parse(
        changedRead["result"]["contents"][0].value("text", ""), nullptr, false);
    ASSERT_FALSE(patch.value("snapshot", true)) << patch.dump(2);
    bool sawToggleChange = false;
    for (const auto& event : patch.value("events", json::array())) {
        const auto fields = event.value("fields", json::object());
        sawToggleChange = sawToggleChange ||
                          (event.value("key", "") == checkboxKey &&
                           fields.contains("properties.Toggle.ToggleState"));
    }
    EXPECT_TRUE(sawToggleChange) << patch.dump(2);

    client.notify(
        "notifications/cancelled",
        json{{"requestId", listenId}, {"reason", "test complete"}});
    Sleep(750);

    auto restored = client.call_tool(
        "toggle", json{{"session", session}, {"element", checkboxRef}}, &isError);
    EXPECT_FALSE(isError) << restored.dump(2);
    auto afterCancellation =
        client.await_notification("notifications/resources/updated", 2);
    EXPECT_TRUE(afterCancellation.is_null())
        << "a cancelled modern subscription must stop its resource watcher";
}

// --- driving the app ----------------------------------------------------

TEST_F(McpSampleFixture, ClickReachesTheApplication) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    const auto readStatus = [&] {
        auto found = client.call_tool(
            "find_elements", json{{"session", session}, {"automationId", "StatusText"}});
        return found["elements"].empty() ? std::string()
                                         : found["elements"][0].value("text", "");
    };

    const auto before = readStatus();
    ASSERT_FALSE(before.empty());

    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    ASSERT_EQ(found["elements"].size(), 1u);

    bool isError = false;
    auto result = client.call_tool(
        "click", json{{"session", session}, {"element", found["elements"][0].value("id", "")}},
        &isError);
    EXPECT_FALSE(isError) << result.dump(2);
    EXPECT_EQ(result.value("method", ""), "InvokePattern")
        << "a button exposes Invoke, so this must not fall back to synthetic input";

    // The assertion that matters: the application actually changed.
    EXPECT_NE(readStatus(), before) << "the click reported success but the app did not react";
}

TEST_F(McpSampleFixture, SetValueWritesThroughTheValuePatternAndReadsBack) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "InputBox"}});
    ASSERT_EQ(found["elements"].size(), 1u) << found.dump(2);
    const auto id = found["elements"][0].value("id", "");

    bool isError = false;
    auto result = client.call_tool(
        "set_value", json{{"session", session}, {"element", id}, {"text", "set through mcp"}},
        &isError);
    EXPECT_FALSE(isError) << result.dump(2);
    EXPECT_EQ(result.value("method", ""), "ValuePattern");

    auto props = client.call_tool(
        "get_element_properties",
        json{{"session", session}, {"element", id},
             {"properties", json::array({"Value.Value"})}});
    EXPECT_EQ(props["properties"].value("Value.Value", ""), "set through mcp");
}

TEST_F(McpSampleFixture, ToggleFlipsStateAndIsObservableAfterwards) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "ReadyCheckBox"}});
    ASSERT_EQ(found["elements"].size(), 1u);
    const auto id = found["elements"][0].value("id", "");

    const auto readState = [&] {
        auto props = client.call_tool(
            "get_element_properties",
            json{{"session", session}, {"element", id},
                 {"properties", json::array({"Toggle.ToggleState"})}});
        return props["properties"].value("Toggle.ToggleState", "");
    };

    const auto before = readState();
    ASSERT_FALSE(before.empty());

    bool isError = false;
    auto result = client.call_tool("toggle", json{{"session", session}, {"element", id}}, &isError);
    EXPECT_FALSE(isError) << result.dump(2);
    EXPECT_NE(readState(), before) << "toggle reported success but the state did not change";

    client.call_tool("toggle", json{{"session", session}, {"element", id}});
    EXPECT_EQ(readState(), before) << "toggling twice should return to the original state";
}

TEST_F(McpSampleFixture, SetExpandedDrivesTheExpandCollapsePattern) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "ChoiceCombo"}});
    if (found["elements"].empty())
        GTEST_SKIP() << "no expandable control in the sample app";
    const auto id = found["elements"][0].value("id", "");

    bool isError = false;
    auto expanded = client.call_tool(
        "set_expanded", json{{"session", session}, {"element", id}, {"expanded", true}}, &isError);
    EXPECT_FALSE(isError) << expanded.dump(2);
    EXPECT_NE(expanded.value("method", "").find("ExpandCollapse"), std::string::npos)
        << expanded.dump(2);

    // Collapse by id would be unreliable: expanding a combo box reparents it,
    // so the element ids from the earlier walk no longer describe the same
    // tree. Re-finding is the documented way to act after a structural change.
    auto again = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "ChoiceCombo"}});
    if (!again["elements"].empty()) {
        client.call_tool("set_expanded",
                         json{{"session", session},
                              {"element", again["elements"][0].value("id", "")},
                              {"expanded", false}});
    }
}

TEST_F(McpSampleFixture, ActionsOnUnknownElementsFailWithoutTouchingTheApp) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    const auto readStatus = [&] {
        auto found = client.call_tool(
            "find_elements", json{{"session", session}, {"automationId", "StatusText"}});
        return found["elements"].empty() ? std::string()
                                         : found["elements"][0].value("text", "");
    };
    const auto before = readStatus();

    bool isError = false;
    auto result = client.call_tool(
        "click", json{{"session", session}, {"element", "uia:99.99.99"}}, &isError);
    EXPECT_TRUE(isError) << result.dump(2);
    EXPECT_FALSE(result.value("error", "").empty());
    EXPECT_EQ(readStatus(), before) << "a failed action must not have side effects";
}

TEST_F(McpSampleFixture, InvalidEnumArgumentsAreRejectedWithAUsefulMessage) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // These two tools fold several verbs into one argument, so an unrecognised
    // value has to say what was expected rather than silently doing nothing.
    bool isError = false;
    auto window = client.call_tool(
        "window_action", json{{"session", session}, {"action", "explode"}}, &isError);
    EXPECT_TRUE(isError);
    EXPECT_NE(window.value("error", "").find("minimize"), std::string::npos) << window.dump(2);

    auto select = client.call_tool(
        "select", json{{"session", session}, {"element", "e0"}, {"mode", "sideways"}}, &isError);
    EXPECT_TRUE(isError);
    EXPECT_NE(select.value("error", "").find("replace"), std::string::npos) << select.dump(2);
}

// --- concurrency ---------------------------------------------------------
//
// rmcp dispatches every request with tokio::spawn onto a multi-threaded
// runtime, so `lvt_api_call` is reentrant in a way the CLI never was: the CLI
// is one process, one action, exit. These tests exist because that is the
// single biggest behavioural difference the server introduces, and nothing
// about lvt_core was originally written with it in mind.

TEST_F(McpSampleFixture, OverlappingRequestsAllGetTheirOwnCorrectAnswer) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // Issue everything before awaiting anything, so the server is working on
    // several at once. The mix is deliberate: get_uia_tree walks on a dedicated
    // MTA thread, screenshot initialises an STA and uses GDI/WIC on the calling
    // thread, and find_elements does neither. Those were the paths most likely
    // to collide.
    //
    // This is a regression test with teeth: without per-target serialization in
    // lvt_api.cpp these calls contend for the target's UI thread and the walk
    // fails outright with "could not read the UI Automation tree" — measured at
    // 2 failures in 5 runs. Enough rounds are issued that the race is reliably
    // hit rather than occasionally.
    struct Pending { int id; std::string tool; };
    std::vector<Pending> pending;
    for (int round = 0; round < 5; ++round) {
        pending.push_back({client.send_request("tools/call",
            json{{"name", "get_uia_tree"},
                 {"arguments", json{{"session", session}, {"depth", 3}}}}), "get_uia_tree"});
        pending.push_back({client.send_request("tools/call",
            json{{"name", "find_elements"},
                 {"arguments", json{{"session", session}, {"automationId", "PrimaryButton"}}}}),
            "find_elements"});
        pending.push_back({client.send_request("tools/call",
            json{{"name", "screenshot"},
                 {"arguments", json{{"session", session}}}}), "screenshot"});
        pending.push_back({client.send_request("tools/call",
            json{{"name", "get_frameworks"},
                 {"arguments", json{{"session", session}}}}), "get_frameworks"});
    }

    for (const auto& [id, tool] : pending) {
        auto response = client.await_response(id);
        ASSERT_FALSE(response.is_null()) << tool << " never answered under concurrency";
        // Every response must carry the id of the request it answers. A mix-up
        // here would mean one caller receiving another's tree, which is the
        // worst plausible outcome and would not be obvious from the payload.
        EXPECT_EQ(response.value("id", -1), id) << "response/request correlation broke";
        ASSERT_TRUE(response.contains("result")) << tool << ": " << response.dump(2);
        EXPECT_FALSE(response["result"].value("isError", false))
            << tool << " failed under concurrency: " << response.dump(2);
    }
}

TEST_F(McpSampleFixture, DisconnectRacingVisualReadKeepsServerAndConnectionSafe) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    for (int round = 0; round < 3; ++round) {
        auto connected = client.call_tool(
            "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
        const auto session = connected.value("session", "");
        ASSERT_FALSE(session.empty());

        // Establish the persistent framework connection first, then race a
        // second read against disconnect. Depending on task scheduling, the
        // read may complete before teardown or be refused after the session
        // is removed; it must never dereference a connection destroyed by
        // the other request, crash the server, or corrupt request routing.
        bool warmError = false;
        client.call_tool("get_visual_tree",
                         json{{"session", session}, {"fast", true}, {"depth", 2}},
                         &warmError);
        ASSERT_FALSE(warmError);

        const int readId = client.send_request(
            "tools/call",
            json{{"name", "get_visual_tree"},
                 {"arguments", json{{"session", session}, {"fast", false}, {"depth", 3}}}});
        const int disconnectId = client.send_request(
            "tools/call",
            json{{"name", "disconnect"}, {"arguments", json{{"session", session}}}});

        auto readResponse = client.await_response(readId);
        auto disconnectResponse = client.await_response(disconnectId);
        ASSERT_FALSE(readResponse.is_null());
        ASSERT_FALSE(disconnectResponse.is_null());
        EXPECT_EQ(readResponse.value("id", -1), readId);
        EXPECT_EQ(disconnectResponse.value("id", -1), disconnectId);
        ASSERT_TRUE(disconnectResponse.contains("result")) << disconnectResponse.dump(2);
        EXPECT_FALSE(disconnectResponse["result"].value("isError", false))
            << disconnectResponse.dump(2);
    }

    auto healthy = client.call_tool("list_apps", json::object());
    EXPECT_TRUE(healthy.contains("apps")) << healthy.dump(2);
}

TEST_F(McpSampleFixture, DisconnectRacingTypedPropertyDoesNotRecreateSession) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    for (int round = 0; round < 5; ++round) {
        const auto session = connect(client);
        ASSERT_FALSE(session.empty());
        auto input = client.call_tool(
            "find_elements",
            json{{"session", session}, {"automationId", "InputBox"}});
        ASSERT_EQ(input["elements"].size(), 1u) << input.dump(2);
        const auto key = input["elements"][0].value("key", "");
        bool isError = false;
        auto properties = client.call_tool(
            "get_editable_properties",
            json{{"session", session}, {"element", key}}, &isError);
        ASSERT_FALSE(isError) << properties.dump(2);
        const auto* descriptor =
            find_property_descriptor(properties, "Value.Value");
        ASSERT_NE(descriptor, nullptr);

        const int propertyId = client.send_request(
            "tools/call",
            json{{"name", "set_property"},
                 {"arguments",
                  json{{"session", session},
                       {"element", key},
                       {"descriptorId", descriptor->value("descriptorId", "")},
                       {"value", typed_property_value(properties, "Value.Value")}}}});
        const int disconnectId = client.send_request(
            "tools/call",
            json{{"name", "disconnect"},
                 {"arguments", json{{"session", session}}}});

        auto disconnectResponse = client.await_response(disconnectId);
        auto propertyResponse = client.await_response(propertyId);
        ASSERT_FALSE(disconnectResponse.is_null());
        ASSERT_FALSE(propertyResponse.is_null());
        ASSERT_TRUE(disconnectResponse.contains("result"))
            << disconnectResponse.dump(2);
        EXPECT_FALSE(disconnectResponse["result"].value("isError", false))
            << disconnectResponse.dump(2);
        ASSERT_TRUE(propertyResponse.contains("result"))
            << propertyResponse.dump(2);

        auto afterDisconnect = client.call_tool(
            "get_editable_properties",
            json{{"session", session}, {"element", key}}, &isError);
        EXPECT_TRUE(isError) << afterDisconnect.dump(2);
        expect_typed_property_session_disconnected(afterDisconnect);
    }

    auto healthy = client.call_tool("list_apps", json::object());
    EXPECT_TRUE(healthy.contains("apps")) << healthy.dump(2);
}

TEST_F(McpSampleFixture, ConcurrentRequestsAcrossTwoSessionsDoNotCrossOver) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    // Two sessions on different windows, interleaved. The session registry is a
    // shared map behind a mutex, so this is what would expose a locking mistake
    // — and, more subtly, an answer built for the wrong target.
    const auto sampleSession = connect(client);
    ASSERT_FALSE(sampleSession.empty());

    auto apps = client.call_tool("list_apps", json::object());
    std::string otherHwnd;
    for (const auto& app : apps["apps"]) {
        const auto name = app.value("processName", "");
        if (name.find("WinUI3Sample") == std::string::npos && !name.empty()) {
            otherHwnd = app.value("hwnd", "");
            break;
        }
    }
    if (otherHwnd.empty())
        GTEST_SKIP() << "no second window to test session isolation with";

    auto other = client.call_tool("connect", json{{"hwnd", otherHwnd}});
    const auto otherSession = other.value("session", "");
    ASSERT_FALSE(otherSession.empty());
    ASSERT_NE(sampleSession, otherSession);

    // Prove the baseline before testing it under load: if the sample session
    // cannot find its own button when nothing else is happening, the failure
    // below is about the fixture, not about concurrency.
    auto baseline = client.call_tool(
        "find_elements", json{{"session", sampleSession}, {"automationId", "PrimaryButton"}});
    ASSERT_EQ(baseline["elements"].size(), 1u)
        << "sequential baseline already broken: " << baseline.dump(2);

    std::vector<std::pair<int, std::string>> pending;
    for (int round = 0; round < 4; ++round) {
        pending.emplace_back(client.send_request("tools/call",
            json{{"name", "find_elements"},
                 {"arguments", json{{"session", sampleSession},
                                    {"automationId", "PrimaryButton"}}}}), sampleSession);
        pending.emplace_back(client.send_request("tools/call",
            json{{"name", "find_elements"},
                 {"arguments", json{{"session", otherSession},
                                    {"automationId", "PrimaryButton"}}}}), otherSession);
    }

    for (const auto& [id, session] : pending) {
        auto response = client.await_response(id);
        ASSERT_FALSE(response.is_null());
        ASSERT_TRUE(response.contains("result")) << response.dump(2);
        auto payload = json::parse(
            response["result"]["content"][0].value("text", "{}"), nullptr, false);
        ASSERT_FALSE(payload.is_discarded());

        // Only the sample app has a PrimaryButton. If a request against the
        // other window ever returned one, an answer built for one session was
        // handed to another.
        const bool isSample = session == sampleSession;
        const size_t matches = payload.value("elements", json::array()).size();
        if (isSample) {
            EXPECT_EQ(matches, 1u) << "the sample session lost its own element. session="
                                   << session << " payload=" << payload.dump();
        } else {
            EXPECT_EQ(matches, 0u)
                << "a session returned another session's element: " << payload.dump(2);
        }
    }
}

TEST(McpServer, ConcurrentRequestsNeverInterleaveOnTheOutputStream) {
    // Every response is written by its own tokio task, so a missing lock in the
    // transport would splice two JSON documents together on stdout. The client
    // parses line by line, so anything spliced would fail to parse — which is
    // exactly what this asserts does not happen.
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    std::vector<int> ids;
    for (int i = 0; i < 30; ++i) {
        // tools/list produces a large response, which is what makes a torn
        // write likely if one were possible.
        ids.push_back(client.send_request("tools/list"));
    }

    for (const int id : ids) {
        auto response = client.await_response(id);
        ASSERT_FALSE(response.is_null()) << "request " << id << " went missing";
        EXPECT_EQ(response.value("id", -1), id);
        ASSERT_TRUE(response.contains("result"));
        EXPECT_FALSE(response["result"]["tools"].empty());
    }
}

TEST(McpServer, SurvivesRequestsArrivingFromSeveralClientThreadsAtOnce) {
    // The transport is fed by one pipe, but a host is free to write from
    // several threads. This drives that case rather than assuming it.
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    constexpr int kThreads = 4;
    constexpr int kPerThread = 8;
    std::vector<std::thread> writers;
    std::mutex idMutex;
    std::vector<int> ids;

    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                const int id = client.send_request(
                    "tools/call", json{{"name", "list_apps"}, {"arguments", json::object()}});
                std::lock_guard<std::mutex> lock(idMutex);
                ids.push_back(id);
            }
        });
    }
    for (auto& writer : writers)
        writer.join();

    ASSERT_EQ(ids.size(), static_cast<size_t>(kThreads * kPerThread));
    for (const int id : ids) {
        auto response = client.await_response(id);
        ASSERT_FALSE(response.is_null()) << "request " << id << " went missing";
        ASSERT_TRUE(response.contains("result")) << response.dump(2);
        EXPECT_FALSE(response["result"].value("isError", false));
    }
}

// --- long-lived server ---------------------------------------------------

TEST_F(McpSampleFixture, ConnectingRepeatedlyDoesNotDegradeTheServer) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    // An MCP server is long-lived, unlike the CLI. Sessions, temp screenshot
    // files and UIA worker threads all accumulate per request, so this walks a
    // connect/use/disconnect cycle enough times that a leak of any of them
    // would show up as a failure or a slowdown rather than going unnoticed.
    for (int i = 0; i < 12; ++i) {
        const auto session = connect(client);
        ASSERT_FALSE(session.empty()) << "connect failed on iteration " << i;

        bool isError = false;
        client.call_tool("find_elements",
                         json{{"session", session}, {"automationId", "PrimaryButton"}}, &isError);
        EXPECT_FALSE(isError) << "iteration " << i;

        client.call_tool("screenshot", json{{"session", session}});
        client.call_tool("disconnect", json{{"session", session}});
    }

    // Still healthy afterwards.
    const auto session = connect(client);
    EXPECT_FALSE(session.empty()) << "the server stopped accepting connections";
}

TEST_F(McpSampleFixture, InlineScreenshotsDoNotLeaveTempFilesBehind) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // An inline screenshot is captured to a temp file and then read back, so a
    // missing cleanup would fill the user's temp directory over a long session.
    const auto countTempShots = [] {
        size_t count = 0;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(fs::temp_directory_path(), ec)) {
            const auto name = entry.path().filename().string();
            if (name.rfind("lvt_mcp_", 0) == 0 && entry.path().extension() == ".png")
                ++count;
        }
        return count;
    };

    const auto before = countTempShots();
    for (int i = 0; i < 5; ++i)
        client.call_tool_content("screenshot", json{{"session", session}});
    EXPECT_EQ(countTempShots(), before) << "inline screenshots left temp files behind";
}

TEST_F(McpSampleFixture, SessionsFailCleanlyOnceTheirWindowIsGone) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    // A window can close at any time under a long-lived server. The session
    // must then fail with something the model can act on rather than crashing
    // the server or returning a stale tree.
    static constexpr wchar_t kClassName[] = L"LvtMcpProbeWindow";
    wil::unique_event ready(wil::EventOptions::ManualReset);
    wil::unique_event destroy(wil::EventOptions::ManualReset);
    HWND probe = nullptr;

    std::thread ui([&] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        probe = CreateWindowExW(0, kClassName, L"lvt mcp probe",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 120, 120, 400, 300,
                                nullptr, nullptr, wc.hInstance, nullptr);
        ready.SetEvent();
        if (!probe)
            return;
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

    ASSERT_TRUE(ready.wait(5000));
    ASSERT_NE(probe, nullptr);

    char hwndText[32];
    snprintf(hwndText, sizeof(hwndText), "0x%llX",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(probe)));
    auto connected = client.call_tool("connect", json{{"hwnd", hwndText}});
    const auto session = connected.value("session", "");
    ASSERT_FALSE(session.empty()) << connected.dump(2);

    destroy.SetEvent();
    ui.join();
    for (int i = 0; i < 50 && IsWindow(probe); ++i)
        Sleep(100);
    ASSERT_FALSE(IsWindow(probe));

    bool isError = false;
    auto result = client.call_tool("get_uia_tree", json{{"session", session}}, &isError);
    EXPECT_TRUE(isError) << "a session whose window closed must not keep answering";
    EXPECT_NE(result.value("error", "").find("closed"), std::string::npos)
        << "the error should say the window closed: " << result.dump(2);

    const std::array<std::pair<const char*, json>, 3> typedCalls{{
        {"get_editable_properties",
         json{{"session", session}, {"element", "uia:1"}}},
        {"set_property",
         json{{"session", session},
              {"element", "uia:1"},
              {"descriptorId", "missing"},
              {"value", "value"}}},
        {"clear_property",
         json{{"session", session},
              {"element", "uia:1"},
              {"descriptorId", "missing"}}},
    }};
    for (const auto& [tool, arguments] : typedCalls) {
        auto typedResult =
            client.call_tool(tool, arguments, &isError);
        EXPECT_TRUE(isError) << tool << ": " << typedResult.dump(2);
        expect_typed_property_session_disconnected(typedResult);
    }

    // And the server must still be usable for everything else.
    auto apps = client.call_tool("list_apps", json::object());
    EXPECT_FALSE(apps["apps"].empty()) << "the server degraded after a target closed";
}

TEST_F(McpSampleFixture, ATruncatedWalkIsReportedRatherThanPresentedAsComplete) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // A deadline this small cannot complete a real walk, which is the point:
    // the danger is not that the walk is cut short but that a caller is not
    // told. "No element matched" from a partial walk means the walk did not
    // finish, and a model that is not told this will report the element as
    // absent — a wrong answer that looks exactly like a right one.
    //
    // Retried a few times because a 1 ms deadline is not *guaranteed* to cut a
    // very small tree short; the assertion must not be skipped away when it
    // does not, or the test would pass with truncation reporting deleted.
    json tight;
    bool sawTruncation = false;
    for (int attempt = 0; attempt < 10 && !sawTruncation; ++attempt) {
        tight = client.call_tool(
            "find_elements",
            json{{"session", session}, {"automationId", "PrimaryButton"}, {"timeoutMs", 1}});
        sawTruncation = tight.contains("truncated");
    }
    ASSERT_TRUE(sawTruncation)
        << "a 1ms deadline never produced a truncated walk, so this test proves nothing: "
        << tight.dump(2);
    EXPECT_NE(tight.value("truncated", "").find("incomplete"), std::string::npos) << tight.dump(2);
    // The note has to be attached to the result the caller actually reads, and
    // the result must still be a success rather than an error: a partial answer
    // is usable, it just cannot be trusted as a negative.
    EXPECT_TRUE(tight.contains("elements")) << tight.dump(2);

    // With a workable deadline there must be no such caveat, or the field would
    // be noise that a model learns to ignore.
    auto normal = client.call_tool(
        "find_elements",
        json{{"session", session}, {"automationId", "PrimaryButton"}, {"timeoutMs", 20000}});
    EXPECT_EQ(normal["elements"].size(), 1u) << normal.dump(2);
    EXPECT_FALSE(normal.contains("truncated"))
        << "a complete walk must not claim to be truncated: " << normal.dump(2);
}

TEST_F(McpSampleFixture, TreeToolsAcceptATimeoutSoTruncationIsActionable) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // Advising a caller to raise timeoutMs is only useful if the tools actually
    // take it; it was missing from the schemas at first while the ABI already
    // honoured it, which made the advice impossible to follow. Every tool that
    // can emit the note must accept the remedy — including hit_test and
    // get_element_properties, which were the two originally left out.
    static constexpr const char* kMustAcceptTimeout[] = {
        "get_uia_tree", "get_visual_tree", "find_elements", "hit_test",
        "get_element_properties", "screenshot",
    };
    auto tools = client.request("tools/list")["result"]["tools"];
    ASSERT_FALSE(tools.empty());
    for (const char* wanted : kMustAcceptTimeout) {
        const json* tool = nullptr;
        for (const auto& candidate : tools) {
            if (candidate.value("name", "") == wanted)
                tool = &candidate;
        }
        ASSERT_NE(tool, nullptr) << wanted << " is not exposed at all";
        const auto properties = (*tool)["inputSchema"].value("properties", json::object());
        EXPECT_TRUE(properties.contains("timeoutMs"))
            << wanted << " must accept timeoutMs so a truncated result can be retried";
    }

    auto result = client.call_tool(
        "get_uia_tree", json{{"session", session}, {"depth", 1}, {"timeoutMs", 20000}});
    EXPECT_TRUE(result.contains("root")) << result.dump(2);
}

// --- review regressions --------------------------------------------------

TEST_F(McpSampleFixture, ScreenshotIdsComeFromTheTreeTheActionToolsResolve) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // Screenshots used to be annotated from the visual tree while every other
    // tool resolves against the UIA tree. Those are two independent `eN`
    // numberings over different nodes, so an id read off the image resolved to
    // an unrelated element — and, because both ids exist, the action succeeded.
    // Reading `e42` off a screenshot and clicking it activated a list item and
    // reported ok:true while the intended checkbox was never touched.
    auto shot = client.call_tool("screenshot", json{{"session", session}});
    ASSERT_TRUE(shot.value("annotated", false)) << shot.dump(2);
    EXPECT_EQ(shot.value("idsFrom", ""), "uia")
        << "screenshot ids must come from the tree the action tools use";

    // The invariant that matters: an id drawn on the image means the same thing
    // to find_elements. Both are UIA ids, so the button's id must round-trip.
    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    ASSERT_EQ(found["elements"].size(), 1u);
    const auto buttonId = found["elements"][0].value("id", "");
    auto props = client.call_tool(
        "get_element_properties",
        json{{"session", session}, {"element", buttonId},
             {"properties", json::array({"AutomationId"})}});
    EXPECT_EQ(props["properties"].value("AutomationId", ""), "PrimaryButton")
        << "the id space screenshots annotate with must resolve the same element";

    // The visual tree is still reachable, but must say so, since its ids are
    // not interchangeable with the ones every other tool takes.
    auto visual = client.call_tool("screenshot", json{{"session", session}, {"uia", false}});
    if (visual.value("annotated", false))
        EXPECT_EQ(visual.value("idsFrom", ""), "visual") << visual.dump(2);
}

TEST_F(McpSampleFixture, ReadOnlyServerWillNotWriteAScreenshotToAChosenPath) {
    SkipIfNotReady();
    const auto victim = fs::temp_directory_path() / "lvt_mcp_victim.txt";
    {
        std::ofstream file(victim);
        file << "important data";
    }

    {
        // --allow-input is described to the model as the boundary, and the
        // read-only instructions say so outright. A path argument creates or
        // truncates that file, so offering it without the gate made a
        // "read-only" server into a file-write primitive.
        McpClient readOnly(false);
        ASSERT_TRUE(readOnly.started());
        ASSERT_TRUE(readOnly.handshake());
        const auto session = connect(readOnly);
        ASSERT_FALSE(session.empty());

        bool isError = false;
        auto result = readOnly.call_tool(
            "screenshot", json{{"session", session}, {"path", victim.string()}}, &isError);
        EXPECT_TRUE(isError) << "a read-only server must refuse to write a file";
        EXPECT_NE(result.value("error", "").find("--allow-input"), std::string::npos)
            << result.dump(2);
    }

    EXPECT_EQ(read_text_file(victim.string()), "important data")
        << "the file was modified by a read-only server";

    {
        // The capability itself is fine, it just belongs behind the gate.
        McpClient full(true);
        ASSERT_TRUE(full.started());
        ASSERT_TRUE(full.handshake());
        const auto session = connect(full);
        ASSERT_FALSE(session.empty());

        const auto shot = (fs::temp_directory_path() / "lvt_mcp_allowed.png").string();
        std::error_code ec;
        fs::remove(shot, ec);
        auto result = full.call_tool("screenshot", json{{"session", session}, {"path", shot}});
        EXPECT_EQ(result.value("path", ""), shot) << result.dump(2);
        EXPECT_TRUE(fs::exists(shot));
        fs::remove(shot, ec);
    }

    std::error_code ec;
    fs::remove(victim, ec);
}

TEST_F(McpSampleFixture, ConnectRefusesAnAmbiguousTitleInsteadOfGuessing) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    // A second instance, so the title matches two windows. Connecting by title
    // used to bind to matches[0] silently, sending every later call in that
    // session to an arbitrary one of them — while the process-name branch right
    // beside it deliberately refused to guess.
    STARTUPINFOA si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::string cmd = WINUI3_SAMPLE_EXE_PATH;
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        fs::path(WINUI3_SAMPLE_EXE_PATH).parent_path().string().c_str(), &si, &pi))
        GTEST_SKIP() << "could not launch a second sample instance";
    wil::unique_process_handle second(pi.hProcess);
    wil::unique_handle secondThread(pi.hThread);
    auto killSecond = wil::scope_exit([&] { TerminateProcess(second.get(), 0); });
    WaitForInputIdle(second.get(), 10000);
    Sleep(2500);

    bool isError = false;
    auto result = client.call_tool(
        "connect", json{{"title", "LVT WinUI3 Sample"}}, &isError);

    // Confirm the premise before judging the behaviour: if only one window is
    // actually visible there is nothing ambiguous, and skipping here would
    // otherwise be indistinguishable from the refusal having been deleted.
    auto apps = client.call_tool("list_apps", json{{"title", "LVT WinUI3 Sample"}});
    const size_t matching = apps["apps"].size();
    if (matching < 2)
        GTEST_SKIP() << "only " << matching << " window matched, so nothing was ambiguous";

    ASSERT_TRUE(isError)
        << matching << " windows matched the title, so connect must refuse rather than pick one: "
        << result.dump(2);
    EXPECT_NE(result.value("error", "").find("several windows"), std::string::npos)
        << "an ambiguous title must be reported, not resolved arbitrarily: " << result.dump(2);
    // The candidates have to be listed, or the caller cannot act on the refusal.
    EXPECT_NE(result.value("error", "").find("hwnd"), std::string::npos) << result.dump(2);
}

TEST_F(McpSampleFixture, ScreenshotRefusesAnElementScopeItCannotResolve) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // Asking to annotate one dialog and silently getting the whole window back
    // gives the caller no way to tell their scope was ignored.
    bool isError = false;
    auto result = client.call_tool(
        "screenshot", json{{"session", session}, {"element", "e9999"}}, &isError);
    EXPECT_TRUE(isError) << "an unresolvable scope must be reported: " << result.dump(2);
    EXPECT_NE(result.value("error", "").find("not found"), std::string::npos) << result.dump(2);
}

TEST_F(McpSampleFixture, GetElementPropertiesReportsATruncatedWalkAndMissingProperties) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // This is the tool most likely to be called with a specific id in hand, so
    // a flat "not found" from a walk that never finished is the most damaging
    // place for the false negative.
    bool isError = false;
    auto tight = client.call_tool(
        "get_element_properties",
        json{{"session", session}, {"element", "e40"}, {"timeoutMs", 1}}, &isError);
    // Retried, because a 1 ms deadline does not *guarantee* a partial walk; the
    // assertion must not be skipped away when it happens to complete, or this
    // would pass with truncation reporting removed.
    for (int attempt = 0; attempt < 10 && !isError; ++attempt) {
        tight = client.call_tool(
            "get_element_properties",
            json{{"session", session}, {"element", "e40"}, {"timeoutMs", 1}}, &isError);
    }
    ASSERT_TRUE(isError) << "a 1ms deadline never failed to resolve, so this proves nothing: "
                         << tight.dump(2);
    EXPECT_NE(tight.value("error", "").find("incomplete"), std::string::npos)
        << "not-found from a partial walk must say the walk was partial: " << tight.dump(2);

    // A property the element does not have must be distinguishable from one
    // whose value happens to be empty.
    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    ASSERT_EQ(found["elements"].size(), 1u);
    auto props = client.call_tool(
        "get_element_properties",
        json{{"session", session},
             {"element", found["elements"][0].value("id", "")},
             {"properties", json::array({"AutomationId", "NoSuchPropertyAtAll"})}});
    EXPECT_EQ(props["properties"].value("AutomationId", ""), "PrimaryButton");
    ASSERT_TRUE(props.contains("notPresent")) << props.dump(2);
    EXPECT_EQ(props["notPresent"].size(), 1u);
    EXPECT_EQ(props["notPresent"][0], "NoSuchPropertyAtAll");
}

TEST(McpServer, BlockingToolCallsDoNotStarveTheServer) {
    // rmcp dispatches each request as its own task and the task reading stdin
    // lives on the same runtime, so a synchronous FFI call made directly on a
    // worker thread blocks it. With two workers, two concurrent blocking calls
    // stopped the server reading its stdin at all: an unrelated list_apps went
    // from 0.07s to 11.14s. Every FFI call now runs on the blocking pool.
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    auto apps = client.call_tool("list_apps", json::object());
    ASSERT_FALSE(apps["apps"].empty());
    auto connected = client.call_tool(
        "connect", json{{"hwnd", apps["apps"][0].value("hwnd", "")}});
    const auto session = connected.value("session", "");
    if (session.empty())
        GTEST_SKIP() << "could not connect to a window to block on";

    // Four waits that will each sit on their deadline, well over the two worker
    // threads the runtime is built with.
    std::vector<int> blockers;
    for (int i = 0; i < 4; ++i) {
        blockers.push_back(client.send_request(
            "tools/call",
            json{{"name", "wait_for"},
                 {"arguments", json{{"session", session},
                                    {"element", "uia:99.99.99"},
                                    {"timeoutMs", 8000}}}}));
    }
    Sleep(500);  // let them be picked up

    const auto start = GetTickCount64();
    auto quick = client.request("tools/list");
    const auto elapsed = GetTickCount64() - start;

    ASSERT_FALSE(quick.is_null()) << "the server stopped answering while calls were in flight";
    EXPECT_LT(elapsed, 3000u)
        << "an unrelated request waited " << elapsed
        << "ms behind blocking calls; the transport is being starved";

    for (const int id : blockers)
        client.await_response(id);
}

TEST_F(McpSampleFixture, AUiaSessionRefusesVisualReferencesInsteadOfGuessing) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // lvt used to bridge a visual reference here: resolve it in the visual
    // tree, then find the UIA element it corresponded to by identity, then by
    // text, then by position. That is a heuristic making a choice inside an
    // action, where the caller cannot see it -- and when it chose wrong it
    // clicked a different control and reported success. Modes replaced it, so
    // this is now a refusal, and the mirror image of a visual session refusing
    // "uia:e6".
    auto visual = client.call_tool("get_visual_tree", json{{"session", session}});
    if (!visual.contains("root"))
        GTEST_SKIP() << "the visual tree is unavailable here";

    std::vector<const json*> all;
    collect_json_elements(visual["root"], all);
    const json* labelled = nullptr;
    for (const auto* element : all) {
        if (element->value("text", "") == "Primary action" && !element->value("key", "").empty()) {
            labelled = element;
            break;
        }
    }
    if (!labelled)
        GTEST_SKIP() << "no labelled visual element to act on";

    const auto before = status_text(client, session);

    // All three spellings of "this came from the visual tree" are refused: the
    // qualified ref, the self-describing durable key, and a bare id the caller
    // has explicitly said to read against the visual tree.
    const std::vector<std::pair<std::string, json>> forms{
        {"qualified ref", json{{"element", labelled->value("ref", "")}}},
        {"durable key", json{{"element", labelled->value("key", "")}}},
        {"bare id with uia:false",
         json{{"element", labelled->value("id", "")}, {"uia", false}}},
    };

    for (const auto& [label, extra] : forms) {
        json args{{"session", session}};
        args.update(extra);
        bool isError = false;
        auto result = client.call_tool("click", args, &isError);
        EXPECT_TRUE(isError) << label << " was accepted by a uia session: " << result.dump(2);
        const auto message = result.value("error", "");
        EXPECT_NE(message.find("visual"), std::string::npos) << label << ": " << message;
        // A refusal has to say how to do what was asked, or it just blocks.
        EXPECT_NE(message.find("uia mode"), std::string::npos) << label << ": " << message;
    }

    // And nothing was driven while all that was refused.
    EXPECT_EQ(status_text(client, session), before)
        << "a refused action still reached the application";
}

TEST_F(McpSampleFixture, ElementsCarryAQualifiedRefNamingTheirTree) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // `eN` is numbered per tree, so the same control is a different number in
    // each — Okta Verify's "Go back" is e33 in the visual tree and e15 in the
    // UIA one. A bare id therefore cannot say where it came from, which is how
    // a reference copied from one tree came to be resolved against the other.
    // Every element carries a qualified `ref` alongside it for that reason.
    auto uiaTree = client.call_tool("get_uia_tree", json{{"session", session}, {"depth", 2}});
    ASSERT_TRUE(uiaTree.contains("root")) << uiaTree.dump(2);
    EXPECT_EQ(uiaTree.value("tree", ""), "uia");
    EXPECT_EQ(uiaTree["root"].value("ref", ""),
              "uia:" + uiaTree["root"].value("id", ""));

    auto visualTree = client.call_tool("get_visual_tree", json{{"session", session}, {"depth", 2}});
    if (visualTree.contains("root")) {
        EXPECT_EQ(visualTree.value("tree", ""), "visual");
        EXPECT_EQ(visualTree["root"].value("ref", ""),
                  "visual:" + visualTree["root"].value("id", ""));
    }

    // find_elements too, since that is where ids usually come from.
    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    ASSERT_EQ(found["elements"].size(), 1u);
    EXPECT_EQ(found.value("tree", ""), "uia");
    EXPECT_EQ(found["elements"][0].value("ref", ""),
              "uia:" + found["elements"][0].value("id", ""));
}

TEST_F(McpSampleFixture, AQualifiedRefResolvesAgainstItsOwnTree) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto visualTree = client.call_tool("get_visual_tree", json{{"session", session}});
    if (!visualTree.contains("root"))
        GTEST_SKIP() << "the visual tree is unavailable here";

    std::vector<const json*> all;
    collect_json_elements(visualTree["root"], all);
    const json* labelled = nullptr;
    for (const auto* element : all) {
        if (element->value("text", "") == "Primary action") {
            labelled = element;
            break;
        }
    }
    if (!labelled)
        GTEST_SKIP() << "no labelled visual element";

    // The qualified form carries the tree with it, so no extra argument is
    // needed and the default cannot send it to the wrong place.
    auto props = client.call_tool(
        "get_element_properties",
        json{{"session", session}, {"element", labelled->value("ref", "")}});
    EXPECT_EQ(props.value("tree", ""), "visual") << props.dump(2);

    // And the same id without the qualifier goes to the default tree, which is
    // a *different* element — the ambiguity the qualified form removes.
    auto bare = client.call_tool(
        "get_element_properties",
        json{{"session", session}, {"element", labelled->value("id", "")}});
    EXPECT_EQ(bare.value("tree", ""), "uia") << bare.dump(2);
}

TEST_F(McpSampleFixture, AQualifiedRefFromTheWrongTreeIsRefusedNotMisresolved) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // Scoping a UIA tree read to a visual reference would otherwise show a
    // completely different subtree while looking like it worked.
    bool isError = false;
    auto result = client.call_tool(
        "get_uia_tree", json{{"session", session}, {"element", "visual:e3"}}, &isError);
    EXPECT_TRUE(isError) << result.dump(2);
    EXPECT_NE(result.value("error", "").find("visual tree"), std::string::npos) << result.dump(2);
}

TEST_F(McpSampleFixture, AQualifiedRefIsAcceptedOnlyByItsOwnTreesSession) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // The positive half of the rule. A uia session drives a "uia:" reference
    // through patterns and says which pattern it used; the refusal of the other
    // tree's references is only defensible if this keeps working.
    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    ASSERT_EQ(found["elements"].size(), 1u) << found.dump(2);
    const auto ref = found["elements"][0].value("ref", "");
    EXPECT_EQ(ref.rfind("uia:", 0), 0u) << "expected a UIA reference, got " << ref;

    const auto before = status_text(client, session);
    bool isError = false;
    auto result = client.call_tool(
        "click", json{{"session", session}, {"element", ref}}, &isError);
    EXPECT_FALSE(isError) << result.dump(2);
    EXPECT_EQ(result.value("method", ""), "InvokePattern")
        << "a uia session must act through patterns: " << result.dump(2);
    EXPECT_NE(status_text(client, session), before)
        << "the click reported success but the app did not react";

    // Nothing is left over from the bridge: a successful action reports the
    // pattern it used and nothing about resolving between trees.
    EXPECT_FALSE(result.contains("resolvedVia"))
        << "references are no longer translated between trees: " << result.dump(2);
}

TEST_F(McpSampleFixture, CorrelationCoversRealControlsNotJustTextNodes) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // Correlation matches on identity, and identity matching once looked up a
    // property no visual provider emits ("AutomationId"), so the path never ran
    // for any real control -- while the only test that existed picked a
    // TextBlock label and stayed green. Each control below carries x:Name,
    // surfaced as "name", which is what the UIA AutomationId is built from.
    //
    // This is also the scenario correlation exists for now that references are
    // never translated: answering "is the control I can see actually
    // automatable, and as what?" in one read.
    static constexpr const char* kControls[] = {
        "InputBox", "PrimaryButton", "ReadyCheckBox", "LevelSlider", "ChoiceCombo",
    };

    auto visual = client.call_tool(
        "get_visual_tree", json{{"session", session}, {"correlate", true}});
    if (!visual.contains("root"))
        GTEST_SKIP() << "the visual tree is unavailable here";
    std::vector<const json*> all;
    collect_json_elements(visual["root"], all);

    int looked = 0;
    int correlated = 0;
    for (const char* wanted : kControls) {
        const json* element = nullptr;
        for (const auto* candidate : all) {
            const auto properties = candidate->value("properties", json::object());
            if (properties.value("name", "") == wanted) {
                element = candidate;
                break;
            }
        }
        if (!element)
            continue;
        ++looked;
        const auto own = element->value("uiaRef", "");
        EXPECT_FALSE(own.empty())
            << wanted << " (visual " << element->value("id", "") << " "
            << element->value("type", "")
            << ") has no counterpart of its own; identity matching is not working: "
            << element->dump(2);
        if (own.empty())
            continue;
        ++correlated;

        // A counterpart is only useful if it is real, so spend it: the whole
        // point is that the caller can take this reference to a uia session.
        auto properties = client.call_tool(
            "get_element_properties", json{{"session", session}, {"element", own}});
        EXPECT_EQ(properties.value("tree", ""), "uia") << properties.dump(2);
    }

    ASSERT_GT(looked, 2) << "the sample app did not expose the named controls";
    EXPECT_EQ(correlated, looked) << "only " << correlated << " of " << looked
                                  << " named controls correlated";
}

TEST_F(McpSampleFixture, WaitGoneOnAVisualRefSucceedsWhenTheElementIsAlreadyAbsent) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // wait-gone's success condition is the element being absent, so bridging it
    // first threw "not found in either tree" in exactly the case the caller was
    // waiting for — breaking the natural close-then-wait pattern.
    const auto start = GetTickCount64();
    bool isError = false;
    auto result = client.call_tool(
        "wait_for",
        json{{"session", session},
             {"element", "wpf|Button|NeverExisted"},
             {"gone", true},
             {"timeoutMs", 4000}},
        &isError);
    const auto elapsed = GetTickCount64() - start;

    EXPECT_FALSE(isError) << "an absent element must satisfy wait-gone: " << result.dump(2);
    EXPECT_LT(elapsed, 3500u) << "wait-gone should return at once, not burn its timeout";
}

TEST_F(McpSampleFixture, ActionsAreRefusedByTheAbiWithoutAllowInput) {
    SkipIfNotReady();
    // The header and the docs both say the gate is enforced inside lvt rather
    // than only by withholding tools. It was not: every action method ran
    // regardless, and only the tool router stopped a model reaching them. That
    // left the public C ABI's contract unmet.
    McpClient readOnly(false);
    ASSERT_TRUE(readOnly.started());
    ASSERT_TRUE(readOnly.handshake());
    const auto session = connect(readOnly);
    ASSERT_FALSE(session.empty());

    // The tool is not registered, so this is refused at the protocol level —
    // which is the outer defence.
    auto response = readOnly.request(
        "tools/call",
        json{{"name", "click"}, {"arguments", json{{"session", session}, {"element", "e0"}}}});
    ASSERT_FALSE(response.is_null());
    EXPECT_TRUE(response.contains("error")) << response.dump(2);

    // Observation is still allowed, so the gate is not simply refusing
    // everything.
    bool isError = false;
    readOnly.call_tool("get_uia_tree", json{{"session", session}, {"depth", 1}}, &isError);
    EXPECT_FALSE(isError);
}

TEST_F(McpSampleFixture, ExitsPromptlyEvenWithALongCallInFlight) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    // The sample app deliberately, rather than whatever window happens to be
    // first: this measures shutdown, so the target's tree has to be quick to
    // read or the measurement is really about the target.
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // Tool calls run on the blocking pool and cannot be cancelled, and dropping
    // a tokio runtime waits for blocking tasks to finish. So a host that closed
    // the pipe while a long wait was in flight was left with a process that
    // hung around for the whole timeout — measured at the full 120s.
    client.send_request("tools/call",
                        json{{"name", "wait_for"},
                             {"arguments", json{{"session", session},
                                                {"element", "uia:99.99.99"},
                                                {"timeoutMs", 120000}}}});
    Sleep(800);  // let it start

    const auto start = GetTickCount64();
    client.shutdown();
    const auto elapsed = GetTickCount64() - start;

    EXPECT_LT(elapsed, 15000u)
        << "the server took " << elapsed
        << "ms to exit after the client disconnected; shutdown should not wait for an "
           "in-flight call";
    EXPECT_EQ(client.exit_code(), 0u);
}

TEST_F(McpSampleFixture, CorrelationMapsVisualElementsToTheirUiaCounterpart) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // The two trees are not one tree numbered twice. The sample has ~74 UIA
    // nodes against ~314 visual ones, and a single button is one UIA element
    // but three visual ones — Button, its ContentPresenter, its TextBlock. No
    // renumbering could make those line up, so the relationship has to be
    // computed and reported rather than assumed away.
    auto plain = client.call_tool("get_visual_tree", json{{"session", session}});
    if (!plain.contains("root"))
        GTEST_SKIP() << "the visual tree is unavailable here";
    std::vector<const json*> plainNodes;
    collect_json_elements(plain["root"], plainNodes);
    for (const auto* node : plainNodes) {
        ASSERT_FALSE(node->contains("uiaRef"))
            << "correlation costs an extra walk, so it must be opt-in: " << node->dump();
    }

    auto correlated = client.call_tool(
        "get_visual_tree", json{{"session", session}, {"correlate", true}});
    ASSERT_TRUE(correlated.contains("root")) << correlated.dump(2);
    ASSERT_TRUE(correlated.contains("correlated")) << correlated.dump(2);

    std::vector<const json*> nodes;
    collect_json_elements(correlated["root"], nodes);
    ASSERT_GT(nodes.size(), 20u);

    std::map<std::string, int> perCounterpart;
    int withOwnRef = 0;
    int withAncestorRef = 0;
    for (const auto* node : nodes) {
        const auto own = node->value("uiaRef", "");
        const auto ancestor = node->value("uiaAncestorRef", "");
        if (!own.empty()) {
            ++withOwnRef;
            ++perCounterpart[own];
            EXPECT_EQ(own.rfind("uia:", 0), 0u) << "a counterpart must be a UIA reference: " << own;
        }
        if (!ancestor.empty()) {
            ++withAncestorRef;
            // The two are mutually exclusive: a node either has a counterpart
            // of its own or is reported as sitting inside one.
            EXPECT_TRUE(own.empty())
                << "a node reported both its own counterpart and an ancestor's: " << node->dump();
        }
    }
    EXPECT_GT(withOwnRef, 0) << "nothing correlated at all";
    EXPECT_GT(withAncestorRef, 0)
        << "no node was reported as sitting inside a correlated control, which is what most of "
           "a visual tree is";

    // The assertion that matters, and the one this test originally lacked:
    // `uiaRef` is documented as the thing you can act on, so **no two distinct
    // visual elements may claim the same one**. Before this was enforced, all
    // 28 of the sample's ListViewItem nodes advertised the ListView itself,
    // and clicking "item 002" clicked the middle of the whole list and
    // reported success.
    for (const auto& [ref, count] : perCounterpart) {
        EXPECT_EQ(count, 1) << count << " different visual elements all claim " << ref
                            << " as their own counterpart; at most one of them can be right";
    }

    // Repeated list rows are the case that exposed it, so check them directly
    // rather than relying on the aggregate above.
    std::vector<const json*> rows;
    for (const auto* node : nodes) {
        if (node->value("type", "").find("ListViewItem") != std::string::npos)
            rows.push_back(node);
    }
    if (rows.size() > 1) {
        std::set<std::string> rowRefs;
        for (const auto* row : rows) {
            const auto own = row->value("uiaRef", "");
            if (!own.empty())
                rowRefs.insert(own);
        }
        EXPECT_EQ(rowRefs.size(), rows.size() - (rows.size() - rowRefs.size()))
            << "sanity";
        for (const auto& ref : rowRefs) {
            int claimants = 0;
            for (const auto* row : rows)
                claimants += row->value("uiaRef", "") == ref ? 1 : 0;
            EXPECT_EQ(claimants, 1)
                << claimants << " list rows all claim " << ref << " as their own counterpart";
        }
    }

    // And the counterpart is directly actionable — no second lookup, no flag.
    const json* button = nullptr;
    for (const auto* node : nodes) {
        const auto properties = node->value("properties", json::object());
        if (properties.value("name", "") == "PrimaryButton" && !node->value("uiaRef", "").empty()) {
            button = node;
            break;
        }
    }
    ASSERT_NE(button, nullptr) << "the sample's button was not correlated";

    const auto before = status_text(client, session);
    bool isError = false;
    auto result = client.call_tool(
        "click", json{{"session", session}, {"element", button->value("uiaRef", "")}}, &isError);
    EXPECT_FALSE(isError) << result.dump(2);
    // Acting on the counterpart needs no bridging, because it is already a UIA
    // reference.
    EXPECT_FALSE(result.contains("resolvedVia")) << result.dump(2);
    EXPECT_NE(status_text(client, session), before)
        << "the correlated counterpart did not actually drive the control";
}

// --- modes ---------------------------------------------------------------
//
// A session declares which tree it speaks and therefore how it drives the app:
// UI Automation knows what a control *is*, so it acts through patterns; the
// visual tree knows where things *are*, so it acts through real input. Keeping
// them apart is what stops a reference ever being resolved against the tree it
// did not come from.

TEST_F(McpSampleFixture, VisualModeDrivesTheAppWithRealInput) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    auto visual = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
    const auto visualSession = visual.value("session", "");
    ASSERT_FALSE(visualSession.empty()) << visual.dump(2);
    EXPECT_EQ(visual.value("mode", ""), "visual") << "the session should report its mode";

    // A second session on the same window in the default mode, used to observe
    // the effect — which also demonstrates that modes can be mixed.
    const auto uiaSession = connect(client);
    ASSERT_FALSE(uiaSession.empty());

    auto tree = client.call_tool("get_visual_tree", json{{"session", visualSession}});
    ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
    std::vector<const json*> all;
    collect_json_elements(tree["root"], all);
    const json* button = nullptr;
    for (const auto* element : all) {
        const auto properties = element->value("properties", json::object());
        if (properties.value("name", "") == "PrimaryButton") {
            button = element;
            break;
        }
    }
    // Not a skip. The visual tree carrying x:Name as `properties.name` is the
    // precondition this whole mode is built on, so its absence is a failure to
    // report, not a reason to pass quietly.
    ASSERT_NE(button, nullptr)
        << "the sample's PrimaryButton is not in the visual tree, so visual mode has nothing "
           "to act on";

    const auto before = status_text(client, uiaSession);
    ASSERT_FALSE(before.empty());
    bool isError = false;
    auto result = client.call_tool(
        "click", json{{"session", visualSession}, {"element", button->value("ref", "")}},
        &isError);
    EXPECT_FALSE(isError) << result.dump(2);
    // The distinguishing property: it really was input, not a pattern call.
    EXPECT_EQ(result.value("method", ""), "SendInput") << result.dump(2);
    EXPECT_EQ(result.value("mode", ""), "visual");
    EXPECT_TRUE(result.contains("at")) << "a geometric click should say where it landed";
    // Synthetic input needs the window on top, and that changes what the user
    // sees, so it has to be reported.
    EXPECT_TRUE(result.value("broughtToForeground", false)) << result.dump(2);

    EXPECT_NE(status_text(client, uiaSession), before)
        << "the click reported success but the app did not react";
}

TEST_F(McpSampleFixture, ModesRefuseEachOthersReferences) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    auto visual = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
    const auto visualSession = visual.value("session", "");
    ASSERT_FALSE(visualSession.empty());

    // The whole point of separating the modes: a reference from the other tree
    // is refused with an explanation, never quietly matched to something else.
    bool isError = false;
    auto result = client.call_tool(
        "click", json{{"session", visualSession}, {"element", "uia:e6"}}, &isError);
    EXPECT_TRUE(isError) << result.dump(2);
    EXPECT_NE(result.value("error", "").find("visual mode"), std::string::npos) << result.dump(2);
    EXPECT_NE(result.value("error", "").find("uia"), std::string::npos)
        << "the message should say how to do what was asked: " << result.dump(2);

    // The qualified form is the easy case. The dangerous one is a *bare* `eN`
    // read off a UIA tree and then used in a visual session: there is nothing
    // in the string to refuse, and `e15` exists in both trees while meaning
    // different controls in each. This is precisely how a "Go back" button in
    // one tree became an unrelated element in the other.
    //
    // Two things have to hold for that to be safe, and both are asserted here.
    auto uiaTree = client.call_tool("get_uia_tree", json{{"session", visualSession}});
    ASSERT_TRUE(uiaTree.contains("root")) << uiaTree.dump(2);
    std::vector<const json*> uiaNodes;
    collect_json_elements(uiaTree["root"], uiaNodes);
    ASSERT_GT(uiaNodes.size(), 1u) << uiaTree.dump(2);

    // First: the bare form is never what a caller is handed. Every element
    // carries a qualified `ref`, so following the protocol cannot produce the
    // ambiguity in the first place.
    for (const auto* node : uiaNodes) {
        ASSERT_EQ(node->value("ref", "").rfind("uia:", 0), 0u)
            << "an element from the UIA tree must name its tree: " << node->dump();
    }

    // Second: if a caller supplies the bare form anyway, lvt says which tree
    // it read it against instead of resolving silently. A stated answer can be
    // checked; a silent one cannot.
    const auto bare = uiaNodes[1]->value("id", "");
    ASSERT_FALSE(bare.empty());
    ASSERT_EQ(bare.rfind("e", 0), 0u) << "expected a bare element id, got " << bare;
    auto resolved = client.call_tool(
        "get_element_properties", json{{"session", visualSession}, {"element", bare}});
    if (resolved.contains("tree")) {
        EXPECT_EQ(resolved.value("tree", ""), "visual")
            << "a bare id in a visual session must be read against the visual tree, and said so: "
            << resolved.dump(2);
        // The full-element response nests the qualified reference inside
        // `element`; the named-subset response carries it at the top level.
        const auto echoed = resolved.contains("element") && resolved["element"].is_object()
                                ? resolved["element"].value("ref", "")
                                : resolved.value("ref", "");
        EXPECT_EQ(echoed.rfind("visual:", 0), 0u)
            << "the echoed reference must be qualified so the caller can see the reading: "
            << resolved.dump(2);
    } else {
        // Not found is equally acceptable — what is not acceptable is a
        // confident answer about the wrong tree.
        EXPECT_TRUE(resolved.contains("error")) << resolved.dump(2);
    }
}

TEST_F(McpSampleFixture, CorrelationStillWorksUnderAnElementScope) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto whole = client.call_tool(
        "get_visual_tree", json{{"session", session}, {"correlate", true}});
    if (!whole.contains("root"))
        GTEST_SKIP() << "the visual tree is unavailable here";

    // Find a node deep enough that its correlated ancestor sits above it, then
    // ask for that node as a scope. Correlation used to start its walk at the
    // scoped root, so everything the subtree inherited from above was thrown
    // away and a scoped request reported far less than the same nodes did in
    // an unscoped one — the caller narrowing the output lost the very field
    // they narrowed it to read.
    std::vector<const json*> all;
    collect_json_elements(whole["root"], all);
    const json* scope = nullptr;
    for (const auto* node : all) {
        if (!node->value("uiaAncestorRef", "").empty() && node->contains("children") &&
            !(*node)["children"].empty()) {
            scope = node;
            break;
        }
    }
    ASSERT_NE(scope, nullptr)
        << "no nested node inherited a counterpart, so the scoped case cannot be tested";

    const auto scopeId = scope->value("id", "");
    std::vector<const json*> expected;
    collect_json_elements(*scope, expected);
    int expectedCorrelated = 0;
    for (const auto* node : expected) {
        if (!node->value("uiaRef", "").empty() || !node->value("uiaAncestorRef", "").empty())
            ++expectedCorrelated;
    }
    ASSERT_GT(expectedCorrelated, 0);

    auto scoped = client.call_tool(
        "get_visual_tree",
        json{{"session", session}, {"element", scopeId}, {"correlate", true}});
    ASSERT_TRUE(scoped.contains("root")) << scoped.dump(2);

    std::vector<const json*> scopedNodes;
    collect_json_elements(scoped["root"], scopedNodes);
    int actualCorrelated = 0;
    for (const auto* node : scopedNodes) {
        if (!node->value("uiaRef", "").empty() || !node->value("uiaAncestorRef", "").empty())
            ++actualCorrelated;
    }
    EXPECT_EQ(actualCorrelated, expectedCorrelated)
        << "scoping the request changed what correlated; the same elements must correlate the "
           "same way however they were asked for";

    // The reported count describes this response, not the whole-tree pass it
    // was computed from, or a scoped caller is told about matches they cannot
    // see.
    EXPECT_EQ(scoped.value("correlated", -1), actualCorrelated) << scoped.dump(2);
    EXPECT_LT(scoped.value("correlated", -1), whole.value("correlated", -1))
        << "a subtree cannot have correlated as much as the whole tree: " << scoped.dump(2);
}

TEST_F(McpSampleFixture, FindingByPatternInAVisualSessionSaysWhyItCannot) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    auto visual = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
    const auto session = visual.value("session", "");
    ASSERT_FALSE(session.empty());

    // Patterns describe what a control can *do*, which only UI Automation
    // knows; the visual tree records how it is built. Filtering a visual tree
    // by pattern therefore matched nothing and came back as an empty list,
    // which reads as "this app has no invokable controls" — a statement about
    // the app, when the truth is a statement about the tree.
    bool isError = false;
    auto result = client.call_tool(
        "find_elements", json{{"session", session}, {"pattern", "Invoke"}}, &isError);
    ASSERT_TRUE(isError) << "an unanswerable query must not come back as an empty answer: "
                         << result.dump(2);
    const auto message = result.value("error", "");
    EXPECT_NE(message.find("pattern"), std::string::npos) << message;
    EXPECT_NE(message.find("uia"), std::string::npos)
        << "the message should say how to ask the question properly: " << message;

    // The rest of find_elements still works in this session, so the refusal is
    // about the one argument and not the tool.
    auto byName = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    EXPECT_FALSE(byName["elements"].empty()) << byName.dump(2);

    // And the same query is answerable in the mode that owns the concept.
    const auto uiaSession = connect(client);
    auto uiaResult = client.call_tool(
        "find_elements", json{{"session", uiaSession}, {"pattern", "Invoke"}});
    EXPECT_FALSE(uiaResult["elements"].empty()) << uiaResult.dump(2);
}

TEST(McpServer, OnlyTheVisualTreeOffersCorrelation) {
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    // Both tree tools shared one argument type, so get_uia_tree advertised
    // `correlate` and then ignored it: the caller asked for counterparts, got
    // a tree without them, and was told nothing. A flag a tool cannot honour
    // should not appear in its schema at all.
    auto tools = client.request("tools/list")["result"]["tools"];
    bool sawUia = false;
    bool sawVisual = false;
    for (const auto& tool : tools) {
        const auto name = tool.value("name", "");
        if (name != "get_uia_tree" && name != "get_visual_tree")
            continue;
        const auto& properties = tool["inputSchema"]["properties"];
        if (name == "get_uia_tree") {
            sawUia = true;
            EXPECT_FALSE(properties.contains("correlate"))
                << "the UIA tree cannot correlate to itself: " << tool["inputSchema"].dump(2);
        } else {
            sawVisual = true;
            EXPECT_TRUE(properties.contains("correlate"))
                << "the visual tree must still offer it: " << tool["inputSchema"].dump(2);
        }
    }
    EXPECT_TRUE(sawUia);
    EXPECT_TRUE(sawVisual);
}

TEST_F(McpSampleFixture, VisualModeSaysWhatItCannotExpress) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    auto visual = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
    const auto visualSession = visual.value("session", "");
    ASSERT_FALSE(visualSession.empty());

    auto tree = client.call_tool("get_visual_tree", json{{"session", visualSession}});
    ASSERT_TRUE(tree.contains("root"));
    std::vector<const json*> all;
    collect_json_elements(tree["root"], all);
    const json* anything = nullptr;
    for (const auto* element : all) {
        if (!element->value("ref", "").empty() &&
            element->value("bounds", json::object()).value("width", 0) > 0) {
            anything = element;
            break;
        }
    }
    ASSERT_NE(anything, nullptr);

    // Toggling and setting a value describe what a control *means*. Geometry
    // cannot express that, and approximating it with a click might do something
    // else entirely — so it is refused, with the alternative named.
    for (const char* pattern : {"toggle", "set_expanded"}) {
        json args{{"session", visualSession}, {"element", anything->value("ref", "")}};
        if (std::string(pattern) == "set_expanded")
            args["expanded"] = true;
        bool isError = false;
        auto result = client.call_tool(pattern, args, &isError);
        EXPECT_TRUE(isError) << pattern << " should not pretend to work: " << result.dump(2);
        EXPECT_NE(result.value("error", "").find("visual mode"), std::string::npos)
            << result.dump(2);
    }
}

TEST_F(McpSampleFixture, UiaModeRemainsTheDefault) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    // Connecting without a mode must keep the pattern-based behaviour, or every
    // existing caller changes meaning.
    auto connected = client.call_tool("connect", json{{"hwnd", hwnd_string()}});
    EXPECT_EQ(connected.value("mode", ""), "uia") << connected.dump(2);
    const auto session = connected.value("session", "");
    ASSERT_FALSE(session.empty());

    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    ASSERT_EQ(found["elements"].size(), 1u);

    const auto before = status_text(client, session);
    bool isError = false;
    auto result = client.call_tool(
        "click", json{{"session", session}, {"element", found["elements"][0].value("ref", "")}},
        &isError);
    EXPECT_FALSE(isError) << result.dump(2);
    EXPECT_EQ(result.value("method", ""), "InvokePattern")
        << "the default mode must still act through patterns: " << result.dump(2);
    EXPECT_NE(status_text(client, session), before);
}

TEST_F(McpSampleFixture, ConnectRejectsAModeItDoesNotKnow) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    bool isError = false;
    auto result = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "telepathy"}}, &isError);
    EXPECT_TRUE(isError) << result.dump(2);
    EXPECT_NE(result.value("error", "").find("uia"), std::string::npos) << result.dump(2);
    EXPECT_NE(result.value("error", "").find("visual"), std::string::npos) << result.dump(2);
}

TEST_F(McpSampleFixture, AVisualSessionNeverHandsOutReferencesItWouldRefuse) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    auto visual = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
    const auto session = visual.value("session", "");
    ASSERT_FALSE(session.empty());

    // Modes only help if the whole session speaks one tree. The read tools
    // originally defaulted to UI Automation whatever the session's mode, so a
    // visual session would answer find_elements with `uia:e6` and then refuse
    // that very reference when it was passed to click — the exact confusion
    // modes exist to remove, just relocated.
    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    EXPECT_EQ(found.value("tree", ""), "visual") << found.dump(2);
    ASSERT_FALSE(found["elements"].empty())
        << "a visual session must be able to find a control by its name: " << found.dump(2);
    const auto ref = found["elements"][0].value("ref", "");
    EXPECT_EQ(ref.rfind("visual:", 0), 0u) << "expected a visual reference, got " << ref;

    // The whole point: what the session gave me, the session accepts.
    const auto uiaSession = connect(client);  // only to observe the effect
    const auto before = status_text(client, uiaSession);
    bool isError = false;
    auto result = client.call_tool("click", json{{"session", session}, {"element", ref}}, &isError);
    EXPECT_FALSE(isError) << "a visual session refused its own reference: " << result.dump(2);
    EXPECT_EQ(result.value("method", ""), "SendInput");
    EXPECT_NE(status_text(client, uiaSession), before);

    // Screenshots and hit-testing have to agree as well, or an id read off an
    // image is unusable in the session that produced it.
    auto shot = client.call_tool("screenshot", json{{"session", session}});
    EXPECT_EQ(shot.value("idsFrom", ""), "visual") << shot.dump(2);

    auto bounds = found["elements"][0]["bounds"];
    auto hit = client.call_tool(
        "hit_test", json{{"session", session},
                         {"x", bounds.value("x", 0) + bounds.value("width", 0) / 2},
                         {"y", bounds.value("y", 0) + bounds.value("height", 0) / 2}});
    // Not conditional: if hit_test fails in a visual session there is no
    // `tree` key, and guarding on its presence would skip the assertion in
    // exactly the case it exists to catch.
    ASSERT_TRUE(hit.contains("tree")) << "hit_test failed in a visual session: " << hit.dump(2);
    EXPECT_EQ(hit.value("tree", ""), "visual") << hit.dump(2);
}

TEST_F(McpSampleFixture, AnExplicitTreeArgumentStillOverridesTheSessionMode) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    auto visual = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
    const auto session = visual.value("session", "");
    ASSERT_FALSE(session.empty());

    // Reading the other tree to *understand* an app is reasonable even when
    // you drive it through this one, so the mode is a default rather than a
    // restriction on inspection.
    auto forced = client.call_tool(
        "find_elements",
        json{{"session", session}, {"automationId", "PrimaryButton"}, {"uia", true}});
    EXPECT_EQ(forced.value("tree", ""), "uia") << forced.dump(2);
    ASSERT_FALSE(forced["elements"].empty());
    EXPECT_EQ(forced["elements"][0].value("ref", "").rfind("uia:", 0), 0u);
}

TEST_F(McpSampleFixture, VisualModeRefusesAnElementScrolledOutOfView) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    auto visual = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
    const auto session = visual.value("session", "");
    ASSERT_FALSE(session.empty());

    // A realized but scrolled-out list item has perfectly valid on-monitor
    // coordinates outside its viewport. Aiming at them delivered the click to
    // whatever was really there — measured landing outside the application
    // entirely, and reported as success.
    auto tree = client.call_tool("get_visual_tree", json{{"session", session}});
    ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
    const auto& rootBounds = tree["root"]["bounds"];
    const int bottom = rootBounds.value("y", 0) + rootBounds.value("height", 0);

    std::vector<const json*> all;
    collect_json_elements(tree["root"], all);
    const json* below = nullptr;
    for (const auto* node : all) {
        const auto& b = (*node)["bounds"];
        if (b.value("height", 0) > 0 && b.value("y", 0) > bottom &&
            !node->value("ref", "").empty()) {
            below = node;
            break;
        }
    }
    if (!below)
        GTEST_SKIP() << "nothing in this tree lies below the window to test with";

    bool isError = false;
    auto result = client.call_tool(
        "click", json{{"session", session}, {"element", below->value("ref", "")}}, &isError);
    EXPECT_TRUE(isError) << "clicking an element outside the window must be refused, not "
                            "delivered to whatever is there: " << result.dump(2);
}

TEST_F(McpSampleFixture, VisualModeSupportsWaiting) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    auto visual = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
    const auto session = visual.value("session", "");
    ASSERT_FALSE(session.empty());

    // Waits observe rather than drive, so they belong in either mode. They were
    // falling through to "no equivalent in visual mode", which left a visual
    // session with no way at all to synchronise after an action — and advised
    // "use click/type here", which is not a wait.
    bool isError = false;
    auto gone = client.call_tool(
        "wait_for",
        json{{"session", session}, {"element", "visual:e99999"}, {"gone", true},
             {"timeoutMs", 3000}},
        &isError);
    EXPECT_FALSE(isError) << "wait-gone on an absent element must succeed: " << gone.dump(2);

    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    ASSERT_FALSE(found["elements"].empty());
    auto present = client.call_tool(
        "wait_for",
        json{{"session", session}, {"element", found["elements"][0].value("ref", "")},
             {"timeoutMs", 3000}},
        &isError);
    EXPECT_FALSE(isError) << "wait-for on a present element must succeed: " << present.dump(2);
}

TEST_F(McpSampleFixture, ARefusedVisualActionLeavesTheDesktopAlone) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    auto visual = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
    const auto session = visual.value("session", "");
    ASSERT_FALSE(session.empty());

    // Refusing used to happen after the window had already been restored and
    // raised, so a call that did nothing the caller asked for still rearranged
    // what they were looking at.
    ShowWindow(s_hwnd, SW_MINIMIZE);
    Sleep(800);
    ASSERT_TRUE(IsIconic(s_hwnd)) << "could not minimize the sample to set up the test";

    bool isError = false;
    client.call_tool("toggle", json{{"session", session}, {"element", "visual:e30"}}, &isError);
    EXPECT_TRUE(isError);
    Sleep(400);
    EXPECT_TRUE(IsIconic(s_hwnd))
        << "a refused action brought the window back — it should not have touched it";

    ShowWindow(s_hwnd, SW_RESTORE);
    Sleep(600);
}

TEST_F(McpSampleFixture, VisualModeClicksAMinimizedWindowOnTheFirstTry) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    auto visual = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
    const auto session = visual.value("session", "");
    ASSERT_FALSE(session.empty());

    // Two bugs met here. bring_to_foreground returned early when the window was
    // already foreground — which a *minimized* window can be — so it never
    // restored it; and the tree was read before the window was raised, so the
    // bounds captured were the -32000 ones a minimized window has. The result
    // was a deterministic "fails the first time, works afterwards".
    ShowWindow(s_hwnd, SW_MINIMIZE);
    Sleep(800);
    ASSERT_TRUE(IsIconic(s_hwnd));

    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    ASSERT_FALSE(found["elements"].empty()) << found.dump(2);

    bool isError = false;
    auto result = client.call_tool(
        "click", json{{"session", session}, {"element", found["elements"][0].value("ref", "")}},
        &isError);
    EXPECT_FALSE(isError) << "the first click on a minimized window failed: " << result.dump(2);
    EXPECT_EQ(result.value("method", ""), "SendInput");
}

TEST_F(McpSampleFixture, VisualModeClicksThroughAWindowSittingOnTopOfTheTarget) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    // Visual mode aims real input at screen coordinates, so it only works if
    // the target is actually the thing drawn there. bring_to_foreground gave
    // the window the input focus but never fixed its z-order, so whenever
    // anything else happened to be covering the app — a chat window, a browser,
    // the terminal running this suite — every click was refused as "covered by
    // another window". It worked on a clean desktop and failed on a real one,
    // which is the worst possible failure mode: it looks like flakiness.
    //
    // Setting that up deliberately is the only way to test it. A second sample
    // instance is used as the obstruction because it has its own message pump.
    STARTUPINFOA si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::string cmd = WINUI3_SAMPLE_EXE_PATH;
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        fs::path(WINUI3_SAMPLE_EXE_PATH).parent_path().string().c_str(), &si, &pi))
        GTEST_SKIP() << "could not launch a second sample instance to act as an obstruction";
    wil::unique_process_handle blocker(pi.hProcess);
    wil::unique_handle blockerThread(pi.hThread);
    auto killBlocker = wil::scope_exit([&] { TerminateProcess(blocker.get(), 0); });
    WaitForInputIdle(blocker.get(), 10000);

    struct Search {
        DWORD pid;
        HWND found;
    } search{pi.dwProcessId, nullptr};
    HWND obstruction = nullptr;
    for (int attempt = 0; attempt < 20 && !obstruction; ++attempt) {
        search.found = nullptr;
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* state = reinterpret_cast<Search*>(lParam);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner != state->pid || !IsWindowVisible(hwnd))
                return TRUE;
            char title[256];
            GetWindowTextA(hwnd, title, sizeof(title));
            if (!strstr(title, "LVT WinUI3 Sample"))
                return TRUE;
            state->found = hwnd;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&search));
        obstruction = search.found;
        if (!obstruction)
            Sleep(500);
    }
    if (!obstruction)
        GTEST_SKIP() << "the second sample instance never showed a window";

    // Find the target element *before* setting up the obstruction. Building a
    // visual tree injects into the target, which can disturb z-order, so doing
    // it afterwards would sometimes undo the very setup being tested.
    auto visual = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
    const auto session = visual.value("session", "");
    ASSERT_FALSE(session.empty());
    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    ASSERT_FALSE(found["elements"].empty()) << found.dump(2);
    const auto& b = found["elements"][0]["bounds"];
    const POINT centre{b.value("x", 0) + b.value("width", 0) / 2,
                       b.value("y", 0) + b.value("height", 0) / 2};

    const auto uiaSession = connect(client);
    const auto before = status_text(client, uiaSession);

    // Park the obstruction exactly over the target and keep asking until it is
    // genuinely the window at the point. Raising another process's window above
    // the *foreground* window is not permitted, and after a preceding test the
    // sample usually is the foreground window — so the target is pushed to the
    // bottom as well, which needs no such permission. Either way the end state
    // is the one being tested: the target is behind something.
    RECT target{};
    ASSERT_TRUE(GetWindowRect(s_hwnd, &target));
    bool covered = false;
    for (int attempt = 0; attempt < 20 && !covered; ++attempt) {
        SetWindowPos(obstruction, HWND_TOP, target.left, target.top,
                     target.right - target.left, target.bottom - target.top, SWP_SHOWWINDOW);
        SetWindowPos(s_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Sleep(250);
        covered = GetAncestor(WindowFromPoint(centre), GA_ROOT) == obstruction;
    }
    // Not a skip. If the obstruction cannot be put on top there is no way to
    // run this test, but silently passing would leave a z-order regression
    // undetected for exactly as long as the setup stayed broken.
    ASSERT_TRUE(covered)
        << "could not place a window over the target, so the occlusion case was never exercised";

    bool isError = false;
    auto result = client.call_tool(
        "click", json{{"session", session}, {"element", found["elements"][0].value("ref", "")}},
        &isError);
    EXPECT_FALSE(isError) << "a click was refused because another window was on top; visual mode "
                             "is supposed to raise the target first: "
                          << result.dump(2);
    EXPECT_EQ(result.value("method", ""), "SendInput") << result.dump(2);

    // Reporting success is not enough — the click has to have reached the app
    // we targeted rather than the one that was covering it.
    EXPECT_NE(status_text(client, uiaSession), before)
        << "the click did not reach the target application";
}

TEST_F(McpSampleFixture, AnOccludedElementNamesWhatIsInTheWay) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    auto visual = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
    const auto session = visual.value("session", "");
    ASSERT_FALSE(session.empty());

    // "covered by another window" is true but unactionable: scrolling a list
    // and closing an app that is sitting on top need opposite responses, and
    // the caller cannot tell which they are looking at. The refusal has to name
    // the window in the way.
    auto tree = client.call_tool("get_visual_tree", json{{"session", session}});
    ASSERT_TRUE(tree.contains("root")) << tree.dump(2);
    const auto& rootBounds = tree["root"]["bounds"];
    const int bottom = rootBounds.value("y", 0) + rootBounds.value("height", 0);

    std::vector<const json*> all;
    collect_json_elements(tree["root"], all);
    const json* below = nullptr;
    for (const auto* node : all) {
        const auto& b = (*node)["bounds"];
        if (b.value("height", 0) > 0 && b.value("width", 0) > 0 && b.value("y", 0) > bottom &&
            !node->value("ref", "").empty()) {
            below = node;
            break;
        }
    }
    if (!below)
        GTEST_SKIP() << "no element sits outside the window, so nothing is occluded";

    bool isError = false;
    auto result = client.call_tool(
        "click", json{{"session", session}, {"element", below->value("ref", "")}}, &isError);
    ASSERT_TRUE(isError) << result.dump(2);
    const auto message = result.value("error", "");
    EXPECT_NE(message.find("covered by"), std::string::npos) << message;
    EXPECT_EQ(message.find("covered by another window"), std::string::npos)
        << "the refusal must name the window in the way, not just say there is one: " << message;
    EXPECT_NE(message.find(".exe"), std::string::npos)
        << "the occluding window should be identified by its process: " << message;
}

TEST_F(McpSampleFixture, CorrelationSaysWhenItCouldNotRead) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    // "nothing correlated" and "the UIA side could not be read" look identical
    // from a count alone, and only one of them is a statement about the app. A
    // deadline too small to finish forces the second.
    auto result = client.call_tool(
        "get_visual_tree", json{{"session", session}, {"correlate", true}, {"timeoutMs", 1}});
    ASSERT_TRUE(result.contains("root")) << result.dump(2);
    if (result.value("correlated", 1u) == 0u) {
        EXPECT_TRUE(result.contains("correlationFailed") || result.contains("correlationPartial"))
            << "zero correlations with no explanation is indistinguishable from a failed read: "
            << result.dump(2);
    }
}

TEST_F(McpSampleFixture, WaitForReturnsPromptlyWhenAlreadySatisfied) {
    SkipIfNotReady();
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    ASSERT_EQ(found["elements"].size(), 1u);

    const auto start = GetTickCount64();
    bool isError = false;
    client.call_tool("wait_for",
                     json{{"session", session},
                          {"element", found["elements"][0].value("id", "")},
                          {"timeoutMs", 5000}},
                     &isError);
    const auto elapsed = GetTickCount64() - start;
    EXPECT_FALSE(isError);
    EXPECT_LT(elapsed, 4000u) << "an element that is already there must not be waited for";
}


TEST(McpServer, EveryToolDeclaresAnOutputSchemaAndAnnotations) {
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());

    // A model decides whether it may call a tool from its annotations, and what
    // shape the answer has from its output schema. Both were missing entirely:
    // every result went out as a JSON string in a text block, so a client had to
    // re-parse a string we already had as JSON, and nothing said which tools
    // change the target application.
    auto tools = client.request("tools/list")["result"]["tools"];
    ASSERT_FALSE(tools.empty());

    for (const auto& tool : tools) {
        const auto name = tool.value("name", "");
        ASSERT_TRUE(tool.contains("outputSchema")) << name << " declares no output schema";
        const auto& schema = tool["outputSchema"];
        // Success or failure: lvt reports refusals as data, so a schema that only
        // described success would be violated by a correct refusal.
        ASSERT_TRUE(schema.contains("anyOf")) << name << ": " << schema.dump(2);
        EXPECT_EQ(schema["anyOf"].size(), 2u) << name;

        ASSERT_TRUE(tool.contains("annotations")) << name << " carries no annotations";
        const auto& annotations = tool["annotations"];
        EXPECT_TRUE(annotations.value("openWorldHint", false))
            << name << " reaches into another application";
        EXPECT_TRUE(annotations.contains("readOnlyHint") || annotations.contains("destructiveHint"))
            << name << " should say whether it changes the target: " << annotations.dump();
    }
}

TEST(McpServer, ReadOnlyToolsAreMarkedReadOnly) {
    McpClient readOnly(false);
    ASSERT_TRUE(readOnly.started());
    ASSERT_TRUE(readOnly.handshake());

    // Everything a read-only server exposes must say it is read-only. Getting
    // this backwards is worse than omitting it: a client that trusts the hint
    // would skip confirmation for something that changes the user's app.
    const auto readOnlyList = readOnly.request("tools/list");
    for (const auto& tool : readOnlyList["result"]["tools"]) {
        EXPECT_TRUE(tool["annotations"].value("readOnlyHint", false))
            << tool.value("name", "") << " is exposed without --allow-input";
    }

    McpClient full(true);
    ASSERT_TRUE(full.started());
    ASSERT_TRUE(full.handshake());
    const auto inspectNames = tool_names(readOnlyList["result"]);
    const auto fullList = full.request("tools/list");
    for (const auto& tool : fullList["result"]["tools"]) {
        const auto name = tool.value("name", "");
        if (std::find(inspectNames.begin(), inspectNames.end(), name) != inspectNames.end())
            continue;
        EXPECT_FALSE(tool["annotations"].value("readOnlyHint", false))
            << name << " changes the target application";
    }
}

TEST(McpServer, AFailureIsStructuredAndMatchesItsSchema) {
    McpClient client(false);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto schemas = output_schemas(client);

    // The failure branch is the half most likely to rot, because the happy path
    // is what gets exercised. Connecting to nothing is a refusal every build can
    // produce without an app.
    auto result = client.call_tool_result("connect", json{{"name", "no.such.process.exe"}});
    ASSERT_TRUE(result.value("isError", false)) << result.dump(2);
    ASSERT_TRUE(result.contains("structuredContent"))
        << "a failure is an answer too, and must travel as structure: " << result.dump(2);

    const auto& structured = result["structuredContent"];
    EXPECT_FALSE(structured.value("ok", true));
    EXPECT_FALSE(structured.value("error", "").empty());

    std::string why;
    EXPECT_TRUE(schema_allows(schemas.at("connect"), structured, why))
        << "a real failure does not match the declared schema: " << why << "\n"
        << structured.dump(2);
}

TEST_F(McpSampleFixture, StructuredContentMatchesTheTextAndTheDeclaredSchema) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto schemas = output_schemas(client);
    const auto session = connect(client);
    ASSERT_FALSE(session.empty());

    auto found = client.call_tool(
        "find_elements", json{{"session", session}, {"automationId", "PrimaryButton"}});
    ASSERT_FALSE(found["elements"].empty()) << found.dump(2);
    const auto ref = found["elements"][0].value("ref", "");
    const auto& bounds = found["elements"][0]["bounds"];

    // One call per distinct response shape, exercised against a real app. A
    // schema is only worth declaring if it is checked against what actually
    // comes back, so this is the test that makes declaring them defensible.
    const std::vector<std::pair<std::string, json>> calls{
        {"list_apps", json::object()},
        {"connect", json{{"hwnd", hwnd_string()}}},
        {"get_frameworks", json{{"session", session}}},
        {"get_uia_tree", json{{"session", session}, {"depth", 3}}},
        {"get_uia_tree_changes", json{{"session", session}}},
        {"get_visual_tree", json{{"session", session}, {"depth", 3}, {"correlate", true}}},
        {"find_elements", json{{"session", session}, {"type", "Button"}}},
        {"get_element_properties", json{{"session", session}, {"element", ref}}},
        {"get_element_properties",
         json{{"session", session}, {"element", ref}, {"properties", json::array({"Name"})}}},
        {"hit_test", json{{"session", session},
                          {"x", bounds.value("x", 0) + bounds.value("width", 0) / 2},
                          {"y", bounds.value("y", 0) + bounds.value("height", 0) / 2}}},
        {"screenshot", json{{"session", session}}},
        {"wait_for", json{{"session", session}, {"element", ref}, {"timeoutMs", 2000}}},
        {"focus", json{{"session", session}, {"element", ref}}},
        {"click", json{{"session", session}, {"element", ref}}},
    };

    for (const auto& [name, args] : calls) {
        auto result = client.call_tool_result(name, args);
        ASSERT_TRUE(result.contains("content")) << name << ": " << result.dump(2);
        ASSERT_TRUE(result.contains("structuredContent"))
            << name << " returned no structured content: " << result.dump(2);

        // The text block exists for clients that predate structured content, so
        // the two must say the same thing. Screenshot is the one exception: its
        // image travels as an image block, and the base64 is stripped from both
        // the text and the structure rather than duplicated.
        std::string text;
        for (const auto& block : result["content"]) {
            if (block.value("type", "") == "text")
                text = block.value("text", "");
        }
        ASSERT_FALSE(text.empty()) << name;
        auto parsed = json::parse(text, nullptr, false);
        ASSERT_FALSE(parsed.is_discarded()) << name << ": text block is not JSON";
        EXPECT_EQ(parsed, result["structuredContent"])
            << name << ": the text and structured copies disagree";

        const auto schema = schemas.find(name);
        ASSERT_NE(schema, schemas.end()) << name << " declares no output schema";
        std::string why;
        EXPECT_TRUE(schema_allows(schema->second, result["structuredContent"], why))
            << name << " does not match its declared output schema: " << why << "\n"
            << result["structuredContent"].dump(2).substr(0, 2000);
    }
}

TEST_F(McpSampleFixture, ARefusedActionIsStructuredToo) {
    SkipIfNotReady();
    McpClient client(true);
    ASSERT_TRUE(client.started());
    ASSERT_TRUE(client.handshake());
    const auto schemas = output_schemas(client);

    auto visual = client.call_tool(
        "connect", json{{"hwnd", hwnd_string()}, {"mode", "visual"}});
    const auto session = visual.value("session", "");
    ASSERT_FALSE(session.empty());

    // The refusals this server does most — a reference from the wrong tree, and
    // an action a mode cannot express — must still come back as data a model can
    // branch on, not only as prose in a text block.
    const std::vector<std::pair<std::string, json>> refusals{
        {"click", json{{"session", session}, {"element", "uia:e6"}}},
        {"toggle", json{{"session", session}, {"element", "visual:e30"}}},
    };

    for (const auto& [name, args] : refusals) {
        auto result = client.call_tool_result(name, args);
        ASSERT_TRUE(result.value("isError", false)) << name << ": " << result.dump(2);
        ASSERT_TRUE(result.contains("structuredContent")) << name << ": " << result.dump(2);
        const auto& structured = result["structuredContent"];
        EXPECT_FALSE(structured.value("ok", true)) << name;
        EXPECT_FALSE(structured.value("error", "").empty()) << name;

        std::string why;
        EXPECT_TRUE(schema_allows(schemas.at(name), structured, why))
            << name << ": " << why << "\n" << structured.dump(2);
    }
}
