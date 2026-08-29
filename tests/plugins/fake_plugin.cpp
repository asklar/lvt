#include "plugin.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>

#ifndef LVT_FAKE_PLUGIN_NAME
#define LVT_FAKE_PLUGIN_NAME "fake-persistent"
#endif

#ifndef LVT_FAKE_PLUGIN_API_VERSION
#define LVT_FAKE_PLUGIN_API_VERSION 2
#endif

#ifndef LVT_FAKE_FRAMEWORK_NAME
#define LVT_FAKE_FRAMEWORK_NAME LVT_FAKE_PLUGIN_NAME
#endif

namespace {

struct FakeConnection {
    HWND hwnd = nullptr;
};

std::atomic<uint32_t> g_getCount{0};
std::atomic<uint32_t> g_openCount{0};
std::atomic<uint32_t> g_detectCount{0};
std::mutex g_logMutex;

bool enabled() {
    char value[8]{};
    const DWORD length =
        GetEnvironmentVariableA("LVT_FAKE_PLUGIN_ENABLE", value, sizeof(value));
    return length > 0 && std::string(value, length) != "0";
}

uint32_t env_uint(const char* name) {
    char value[32]{};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    return length > 0 ? static_cast<uint32_t>(strtoul(value, nullptr, 10)) : 0;
}

bool detection_marker_present() {
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LVT_FAKE_PLUGIN_DETECT_FILE", path,
        static_cast<DWORD>(std::size(path)));
    if (length == 0)
        return true;
    if (length >= std::size(path))
        return false;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

void append_stat(const std::string& line) {
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LVT_FAKE_PLUGIN_STATE", path,
        static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path))
        return;

    std::lock_guard<std::mutex> lock(g_logMutex);
    HANDLE file = CreateFileW(
        path, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    const std::string record = line + "\r\n";
    DWORD written = 0;
    WriteFile(
        file, record.data(), static_cast<DWORD>(record.size()), &written,
        nullptr);
    CloseHandle(file);
}

std::string json_escape(const char* value) {
    std::string escaped;
    if (!value)
        return escaped;
    for (const unsigned char ch : std::string(value)) {
        switch (ch) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch >= 0x20)
                escaped += static_cast<char>(ch);
            break;
        }
    }
    return escaped;
}

int return_tree(
    HWND hwnd, const char* source, const char* filter, uint32_t generation,
    char** jsonOut) {
    if (!jsonOut)
        return 0;

    char hwndText[32]{};
    snprintf(
        hwndText, sizeof(hwndText), "0x%p", static_cast<void*>(hwnd));
    const auto escapedFilter = json_escape(filter);
    const std::string json =
        "[{\"target_hwnd\":\"" + std::string(hwndText) +
        "\",\"children\":[{\"type\":\"FakePluginNode\",\"name\":\"" +
        std::string("fake-node") +
        "\",\"properties\":{\"source\":\"" + source +
        "\",\"filter\":\"" + escapedFilter +
        "\",\"generation\":\"" + std::to_string(generation) +
        "\",\"plugin\":\"" + LVT_FAKE_PLUGIN_NAME + "\"}}]}]";
    auto* result = static_cast<char*>(malloc(json.size() + 1));
    if (!result)
        return 0;
    memcpy(result, json.c_str(), json.size() + 1);
    *jsonOut = result;
    return 1;
}

LvtPluginInfo g_info = {
    sizeof(LvtPluginInfo),
    LVT_FAKE_PLUGIN_API_VERSION,
    LVT_FAKE_PLUGIN_NAME,
    "Deterministic lvt persistent connection test plugin",
};

} // namespace

extern "C" {

__declspec(dllexport) LvtPluginInfo* lvt_plugin_info(void) {
    return &g_info;
}

__declspec(dllexport) int lvt_detect_framework(
    DWORD, HWND, LvtFrameworkDetection* out) {
    if (!enabled() || !out || !detection_marker_present())
        return 0;
    const uint32_t call = ++g_detectCount;
    append_stat("detect " + std::to_string(call));
    if (call == env_uint("LVT_FAKE_PLUGIN_DELAY_DETECT_AT")) {
        const uint32_t delay =
            env_uint("LVT_FAKE_PLUGIN_DETECT_DELAY_MS");
        if (delay)
            Sleep(delay);
    }
    out->struct_size = sizeof(*out);
    out->name = LVT_FAKE_FRAMEWORK_NAME;
    out->version = "test";
    return 1;
}

__declspec(dllexport) int lvt_enrich_tree(
    HWND hwnd, DWORD, const char* elementClassFilter, char** jsonOut) {
    append_stat(
        "enrich filter=" +
        std::string(elementClassFilter ? elementClassFilter : "<null>"));
    return return_tree(hwnd, "one-shot", elementClassFilter, 0, jsonOut);
}

__declspec(dllexport) void lvt_plugin_free(void* pointer) {
    append_stat("free");
    free(pointer);
}

#if defined(LVT_FAKE_PLUGIN_PERSISTENT) || defined(LVT_FAKE_PLUGIN_PARTIAL)

__declspec(dllexport) void* lvt_connection_open(HWND hwnd, DWORD) {
    const uint32_t call = ++g_openCount;
    append_stat("open " + std::to_string(call));
    if (call == env_uint("LVT_FAKE_PLUGIN_FAIL_OPEN_AT")) {
        append_stat("open_failed " + std::to_string(call));
        return nullptr;
    }
    return new FakeConnection{hwnd};
}

__declspec(dllexport) int lvt_connection_get_tree(
    void* connection, const char* elementClassFilter, char** jsonOut) {
    auto* fake = static_cast<FakeConnection*>(connection);
    if (!fake)
        return 0;
    const uint32_t call = ++g_getCount;
    append_stat(
        "get " + std::to_string(call) + " filter=" +
        std::string(elementClassFilter ? elementClassFilter : "<null>"));
    const uint32_t delay = env_uint("LVT_FAKE_PLUGIN_GET_DELAY_MS");
    if (delay)
        Sleep(delay);
    if (call == env_uint("LVT_FAKE_PLUGIN_FAIL_GET_AT")) {
        append_stat("get_failed " + std::to_string(call));
        return 0;
    }
    if (call == env_uint("LVT_FAKE_PLUGIN_MALFORMED_GET_AT")) {
        append_stat("get_malformed " + std::to_string(call));
        *jsonOut = _strdup("42");
        return *jsonOut != nullptr;
    }
    return return_tree(
        fake->hwnd, "persistent", elementClassFilter, call, jsonOut);
}

#endif

#if defined(LVT_FAKE_PLUGIN_PERSISTENT)

__declspec(dllexport) int lvt_connection_poll_events(
    void* connection, LvtConnectionEvent** eventsOut, uint32_t* countOut) {
    if (!connection || !eventsOut || !countOut)
        return 0;
    append_stat("poll");
    *eventsOut = nullptr;
    *countOut = 0;
    if (!env_uint("LVT_FAKE_PLUGIN_EMIT_EVENTS"))
        return 1;

    auto* events = static_cast<LvtConnectionEvent*>(
        malloc(sizeof(LvtConnectionEvent)));
    if (!events)
        return 0;
    *events = {
        sizeof(LvtConnectionEvent),
        "add",
        0xF001,
        0,
        0,
        "FakeEventNode",
        "pushed",
    };
    *eventsOut = events;
    *countOut = 1;
    return 1;
}

__declspec(dllexport) void lvt_connection_events_free(
    LvtConnectionEvent* events, uint32_t) {
    append_stat("events_free");
    free(events);
}

__declspec(dllexport) void lvt_connection_close(void* connection) {
    append_stat("close");
    delete static_cast<FakeConnection*>(connection);
}

#endif

}
