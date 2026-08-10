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

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string get_lvt_path() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    return (fs::path(exePath).parent_path() / "lvt.exe").string();
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
        if (process_ && WaitForSingleObject(process_.get(), 5000) == WAIT_TIMEOUT)
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

    void notify(const std::string& method) {
        write(json{{"jsonrpc", "2.0"}, {"method", method}});
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
        write(json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
        return id;
    }

    json await_response(int id) { return await(id); }

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

    // initialize + initialized, the handshake every session begins with.
    bool handshake(json* serverInfo = nullptr) {
        auto response = request("initialize",
                                json{{"protocolVersion", "2025-06-18"},
                                     {"capabilities", json::object()},
                                     {"clientInfo", json{{"name", "lvt-tests"}, {"version", "1"}}}});
        if (response.is_null() || !response.contains("result"))
            return false;
        if (serverInfo)
            *serverInfo = response["result"];
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
};

std::vector<std::string> tool_names(const json& toolsResult) {
    std::vector<std::string> names;
    for (const auto& tool : toolsResult.value("tools", json::array()))
        names.push_back(tool.value("name", ""));
    std::sort(names.begin(), names.end());
    return names;
}

bool has_tool(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// The sample app is the target for anything that needs known AutomationIds.
class McpSampleFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        const fs::path exe = WINUI3_SAMPLE_EXE_PATH;
        if (!fs::exists(exe))
            return;
        STARTUPINFOA si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        std::string cmd = exe.string();
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            exe.parent_path().string().c_str(), &si, &pi))
            return;
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
    }

    static void TearDownTestSuite() {
        if (s_process)
            TerminateProcess(s_process.get(), 0);
        s_process.reset();
        s_thread.reset();
        s_hwnd = nullptr;
    }

    void SkipIfNotReady() {
        if (!s_hwnd)
            GTEST_SKIP() << "the WinUI3 sample app is not available";
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

    // Connect and return the session id, failing the test if it does not work.
    static std::string connect(McpClient& client) {
        auto result = client.call_tool("connect", json{{"hwnd", hwnd_string()}});
        return result.value("session", "");
    }

    static wil::unique_process_handle s_process;
    static wil::unique_handle s_thread;
    static HWND s_hwnd;
};

wil::unique_process_handle McpSampleFixture::s_process;
wil::unique_handle McpSampleFixture::s_thread;
HWND McpSampleFixture::s_hwnd = nullptr;

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
    auto toFile = client.call_tool("screenshot", json{{"session", session}, {"path", path}});
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
    McpClient client(false);
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
    auto tight = client.call_tool(
        "find_elements",
        json{{"session", session}, {"automationId", "PrimaryButton"}, {"timeoutMs", 1}});

    if (tight.value("elements", json::array()).empty()) {
        EXPECT_TRUE(tight.contains("truncated"))
            << "a walk that found nothing must say whether it actually finished: "
            << tight.dump(2);
        EXPECT_NE(tight.value("truncated", "").find("incomplete"), std::string::npos)
            << tight.dump(2);
    }

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
    // honoured it, which made the advice impossible to follow.
    auto tools = client.request("tools/list")["result"]["tools"];
    for (const auto& tool : tools) {
        const auto name = tool.value("name", "");
        if (name != "get_uia_tree" && name != "find_elements")
            continue;
        const auto properties = tool["inputSchema"].value("properties", json::object());
        EXPECT_TRUE(properties.contains("timeoutMs"))
            << name << " must accept timeoutMs so a truncated result can be retried";
    }

    auto result = client.call_tool(
        "get_uia_tree", json{{"session", session}, {"depth", 1}, {"timeoutMs", 20000}});
    EXPECT_TRUE(result.contains("root")) << result.dump(2);
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
