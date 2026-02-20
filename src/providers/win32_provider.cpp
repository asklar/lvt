#include "win32_provider.h"
#include <vector>

namespace lvt {

static std::string wstr_to_str(const wchar_t* ws, int len = -1) {
    if (!ws || (len == 0)) return {};
    if (len < 0) len = static_cast<int>(wcslen(ws));
    int sz = WideCharToMultiByte(CP_UTF8, 0, ws, len, nullptr, 0, nullptr, nullptr);
    std::string s(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, len, s.data(), sz, nullptr, nullptr);
    return s;
}

static std::string get_window_class(HWND hwnd) {
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    return wstr_to_str(cls);
}

static std::string get_window_text(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len == 0) return {};
    std::wstring buf(len + 1, L'\0');
    GetWindowTextW(hwnd, buf.data(), len + 1);
    return wstr_to_str(buf.c_str(), len);
}

static std::string style_to_string(LONG style) {
    std::string s;
    if (style & WS_OVERLAPPEDWINDOW) s += "WS_OVERLAPPEDWINDOW ";
    if (style & WS_POPUP) s += "WS_POPUP ";
    if (style & WS_CHILD) s += "WS_CHILD ";
    if (style & WS_VISIBLE) s += "WS_VISIBLE ";
    if (style & WS_DISABLED) s += "WS_DISABLED ";
    if (style & WS_MINIMIZE) s += "WS_MINIMIZE ";
    if (style & WS_MAXIMIZE) s += "WS_MAXIMIZE ";
    if (style & WS_HSCROLL) s += "WS_HSCROLL ";
    if (style & WS_VSCROLL) s += "WS_VSCROLL ";
    if (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// Map well-known class names to friendly type names
static std::string classify_window(const std::string& className) {
    if (className == "Button") return "Button";
    if (className == "Edit") return "Edit";
    if (className == "Static") return "Static";
    if (className == "ComboBox") return "ComboBox";
    if (className == "ListBox") return "ListBox";
    if (className == "ScrollBar") return "ScrollBar";
    if (className == "#32770") return "Dialog";
    return "Window";
}

struct EnumChildData {
    std::vector<HWND> children;
    HWND parent;
};

static BOOL CALLBACK enum_direct_children(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<EnumChildData*>(lParam);
    // Only collect direct children
    if (GetParent(hwnd) == data->parent) {
        data->children.push_back(hwnd);
    }
    return TRUE;
}

Element Win32Provider::build(HWND hwnd, int maxDepth) {
    return build_element(hwnd, 0, maxDepth);
}

Element Win32Provider::build_element(HWND hwnd, int depth, int maxDepth) {
    Element el;
    el.nativeHandle = reinterpret_cast<uintptr_t>(hwnd);
    el.framework = "win32";
    el.className = get_window_class(hwnd);
    el.type = classify_window(el.className);
    el.text = get_window_text(hwnd);

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    el.bounds = {rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top};

    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    el.properties["style"] = style_to_string(style);
    el.properties["visible"] = (IsWindowVisible(hwnd)) ? "true" : "false";
    el.properties["enabled"] = (IsWindowEnabled(hwnd)) ? "true" : "false";

    // HWND as hex string for reference
    char hwndBuf[32];
    snprintf(hwndBuf, sizeof(hwndBuf), "0x%p", hwnd);
    el.properties["hwnd"] = hwndBuf;

    // Enumerate direct children
    if (maxDepth < 0 || depth < maxDepth) {
        EnumChildData data{{}, hwnd};
        EnumChildWindows(hwnd, enum_direct_children, reinterpret_cast<LPARAM>(&data));
        for (auto child : data.children) {
            el.children.push_back(build_element(child, depth + 1, maxDepth));
        }
    }

    return el;
}

} // namespace lvt
