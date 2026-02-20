// Integration tests for LVT — require lvt.exe and a running Notepad instance
// Run: ctest --test-dir build -R integration

#include <gtest/gtest.h>
#include <Windows.h>
#include <cstdio>
#include <string>
#include <array>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

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

// Run a command and capture stdout
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
        if (s_pi.hProcess) {
            WaitForInputIdle(s_pi.hProcess, 5000);
        }
        Sleep(3000);  // Extra time for window creation

        // Find the HWND for our Notepad by PID
        s_pid = s_pi.dwProcessId;
    }

    static void TearDownTestSuite() {
        if (s_pi.hProcess) {
            TerminateProcess(s_pi.hProcess, 0);
            CloseHandle(s_pi.hProcess);
            CloseHandle(s_pi.hThread);
        }
        fs::remove(s_temp_file);
    }

    static std::string get_pid_arg() {
        return "--pid " + std::to_string(s_pid);
    }

    static std::string s_temp_file;
    static PROCESS_INFORMATION s_pi;
    static DWORD s_pid;
};

std::string NotepadFixture::s_temp_file;
PROCESS_INFORMATION NotepadFixture::s_pi = {};
DWORD NotepadFixture::s_pid = 0;

// ---- Basic functionality ----

TEST_F(NotepadFixture, CanDumpJsonTree) {
    auto lvt = get_lvt_path();
    auto output = run_command("\"" + lvt + "\" " + get_pid_arg() + " 2>nul");
    ASSERT_FALSE(output.empty()) << "lvt produced no output";

    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "Output is not valid JSON";
    EXPECT_TRUE(j.contains("target"));
    EXPECT_TRUE(j.contains("frameworks"));
    EXPECT_TRUE(j.contains("root"));
}

TEST_F(NotepadFixture, TargetInfo) {
    auto lvt = get_lvt_path();
    auto output = run_command("\"" + lvt + "\" " + get_pid_arg() + " 2>nul");
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
    auto output = run_command("\"" + lvt + "\" " + get_pid_arg() + " --frameworks 2>nul");
    ASSERT_FALSE(output.empty());
    // Notepad should at least have win32
    EXPECT_NE(output.find("win32"), std::string::npos);
}

TEST_F(NotepadFixture, TreeHasElements) {
    auto lvt = get_lvt_path();
    auto output = run_command("\"" + lvt + "\" " + get_pid_arg() + " 2>nul");
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
    auto output = run_command("\"" + lvt + "\" " + get_pid_arg() + " --format xml 2>nul");
    ASSERT_FALSE(output.empty());

    EXPECT_NE(output.find("<LiveVisualTree"), std::string::npos);
    EXPECT_NE(output.find("</LiveVisualTree>"), std::string::npos);
    EXPECT_NE(output.find("id=\"e0\""), std::string::npos);
}

TEST_F(NotepadFixture, DepthLimit) {
    auto lvt = get_lvt_path();
    auto deep = run_command("\"" + lvt + "\" " + get_pid_arg() + " --depth 1 2>nul");
    auto full = run_command("\"" + lvt + "\" " + get_pid_arg() + " 2>nul");

    ASSERT_FALSE(deep.empty());
    ASSERT_FALSE(full.empty());
    // Depth-limited output should be shorter
    EXPECT_LT(deep.size(), full.size());
}

TEST_F(NotepadFixture, ElementSubtree) {
    auto lvt = get_lvt_path();
    auto output = run_command("\"" + lvt + "\" " + get_pid_arg() + " --element e1 2>nul");
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
    auto tmpFile = fs::temp_directory_path() / "lvt_test_screenshot.png";
    fs::remove(tmpFile);

    auto cmd = "\"" + lvt + "\" " + get_pid_arg() + " --screenshot \"" + tmpFile.string() + "\" 2>nul";
    auto output = run_command(cmd);

    // --screenshot without --dump should produce no stdout
    EXPECT_TRUE(output.empty()) << "stdout should be empty with --screenshot only";
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

TEST_F(NotepadFixture, ScreenshotWithDump) {
    auto lvt = get_lvt_path();
    auto tmpFile = fs::temp_directory_path() / "lvt_test_both.png";
    fs::remove(tmpFile);

    auto cmd = "\"" + lvt + "\" " + get_pid_arg() + " --screenshot \"" + tmpFile.string() + "\" --dump 2>nul";
    auto output = run_command(cmd);

    // Should have both tree output and screenshot file
    EXPECT_FALSE(output.empty()) << "stdout should have tree output with --dump";
    EXPECT_TRUE(fs::exists(tmpFile)) << "Screenshot file was not created";

    if (!output.empty()) {
        auto j = json::parse(output, nullptr, false);
        EXPECT_FALSE(j.is_discarded()) << "stdout should be valid JSON";
    }
    fs::remove(tmpFile);
}

TEST_F(NotepadFixture, OutputToFile) {
    auto lvt = get_lvt_path();
    auto tmpFile = fs::temp_directory_path() / "lvt_test_output.json";
    fs::remove(tmpFile);

    auto cmd = "\"" + lvt + "\" " + get_pid_arg() + " --output \"" + tmpFile.string() + "\" 2>nul";
    run_command(cmd);

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
    auto ret = system(("\"" + lvt + "\" >nul 2>nul").c_str());
    EXPECT_NE(ret, 0) << "Should return non-zero with no args";
}

TEST(LvtCli, InvalidHwnd) {
    auto lvt = get_lvt_path();
    auto ret = system(("\"" + lvt + "\" --hwnd 0xDEADBEEF >nul 2>nul").c_str());
    EXPECT_NE(ret, 0) << "Should fail with invalid HWND";
}

TEST(LvtCli, UnknownArg) {
    auto lvt = get_lvt_path();
    auto ret = system(("\"" + lvt + "\" --bogus >nul 2>nul").c_str());
    EXPECT_NE(ret, 0) << "Should fail with unknown argument";
}
