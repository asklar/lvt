#include "input.h"
#include "debug.h"

#include <wil/result.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <unordered_map>

namespace lvt {
namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t");
    if (first == std::string::npos)
        return {};
    const size_t last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

// Named keys. Single characters and digits are resolved separately via
// VkKeyScan so "a" and "7" work without an entry each.
const std::unordered_map<std::string, WORD>& key_names() {
    static const std::unordered_map<std::string, WORD> names = {
        {"enter", VK_RETURN},   {"return", VK_RETURN},
        {"tab", VK_TAB},        {"esc", VK_ESCAPE},      {"escape", VK_ESCAPE},
        {"space", VK_SPACE},    {"spacebar", VK_SPACE},
        {"backspace", VK_BACK}, {"back", VK_BACK},
        {"delete", VK_DELETE},  {"del", VK_DELETE},
        {"insert", VK_INSERT},  {"ins", VK_INSERT},
        {"home", VK_HOME},      {"end", VK_END},
        {"pageup", VK_PRIOR},   {"pgup", VK_PRIOR},
        {"pagedown", VK_NEXT},  {"pgdn", VK_NEXT},
        {"up", VK_UP},          {"down", VK_DOWN},
        {"left", VK_LEFT},      {"right", VK_RIGHT},
        {"printscreen", VK_SNAPSHOT},
        {"apps", VK_APPS},      {"menu", VK_APPS},
        {"capslock", VK_CAPITAL},
    };
    return names;
}

bool is_extended_key(WORD vk) {
    // Extended keys need KEYEVENTF_EXTENDEDKEY or they map to the numpad.
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR:  case VK_NEXT:   case VK_UP:   case VK_DOWN:
    case VK_LEFT:   case VK_RIGHT:  case VK_SNAPSHOT: case VK_APPS:
    case VK_LWIN:   case VK_RWIN:
        return true;
    default:
        return false;
    }
}

void append_key(std::vector<INPUT>& inputs, WORD vk, bool up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) |
                       (is_extended_key(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    inputs.push_back(input);
}

bool dispatch(std::vector<INPUT>& inputs) {
    if (inputs.empty())
        return true;
    const UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    if (sent != inputs.size()) {
        LOG_LAST_ERROR_MSG("SendInput sent %u of %zu events", sent, inputs.size());
        return false;
    }
    return true;
}

std::wstring widen(const std::string& text) {
    if (text.empty())
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), needed);
    return out;
}

} // namespace

bool parse_key_chord(const std::string& text, KeyChord& out) {
    out = {};
    const auto trimmed = trim(text);
    if (trimmed.empty())
        return false;

    // '+' is both the separator and a legitimate key ("Ctrl++" zooms in), so
    // the key is peeled off first and the rest split as modifiers. A dangling
    // separator ("Ctrl+") is an error, not a request for the '+' key.
    std::string keyToken;
    std::string modifierPart;
    if (trimmed.back() == '+') {
        if (trimmed.size() == 1) {
            keyToken = "+";
        } else if (trimmed[trimmed.size() - 2] == '+') {
            keyToken = "+";
            modifierPart = trimmed.substr(0, trimmed.size() - 2);
        } else {
            return false;
        }
    } else {
        const size_t lastPlus = trimmed.rfind('+');
        if (lastPlus == std::string::npos) {
            keyToken = trimmed;
        } else {
            keyToken = trimmed.substr(lastPlus + 1);
            modifierPart = trimmed.substr(0, lastPlus);
        }
    }
    if (keyToken.empty())
        return false;

    if (!modifierPart.empty()) {
        size_t start = 0;
        while (start <= modifierPart.size()) {
            const size_t plus = modifierPart.find('+', start);
            const auto token = trim(modifierPart.substr(
                start, plus == std::string::npos ? std::string::npos : plus - start));
            if (token.empty())
                return false;
            const auto lower = to_lower(token);
            if (lower == "ctrl" || lower == "control")      out.ctrl = true;
            else if (lower == "alt")                        out.alt = true;
            else if (lower == "shift")                      out.shift = true;
            else if (lower == "win" || lower == "meta")     out.win = true;
            else return false;
            if (plus == std::string::npos)
                break;
            start = plus + 1;
        }
    }

    const auto key = trim(keyToken);
    if (key.empty())
        return false;
    const auto lower = to_lower(key);

    // Function keys.
    if (lower.size() >= 2 && lower[0] == 'f' &&
        std::all_of(lower.begin() + 1, lower.end(),
                    [](unsigned char c) { return std::isdigit(c); })) {
        const int n = std::atoi(lower.c_str() + 1);
        if (n < 1 || n > 24)
            return false;
        out.vk = static_cast<WORD>(VK_F1 + n - 1);
        return true;
    }

    const auto& names = key_names();
    const auto it = names.find(lower);
    if (it != names.end()) {
        out.vk = it->second;
        return true;
    }

    // Single character: resolve through the active layout so punctuation and
    // letters both work. VkKeyScanW also reports the shift state it needs.
    const auto wide = widen(key);
    if (wide.size() == 1) {
        const SHORT scan = VkKeyScanW(wide[0]);
        if (scan == -1)
            return false;
        out.vk = static_cast<WORD>(scan & 0xFF);
        if ((scan >> 8) & 1) out.shift = true;
        if ((scan >> 8) & 2) out.ctrl = true;
        if ((scan >> 8) & 4) out.alt = true;
        return true;
    }
    return false;
}

bool parse_key_chords(const std::string& text, std::vector<KeyChord>& out) {
    out.clear();
    size_t start = 0;
    while (start <= text.size()) {
        const size_t sep = text.find_first_of(";,", start);
        const auto piece = trim(text.substr(
            start, sep == std::string::npos ? std::string::npos : sep - start));
        if (!piece.empty()) {
            KeyChord chord;
            if (!parse_key_chord(piece, chord))
                return false;
            out.push_back(chord);
        }
        if (sep == std::string::npos)
            break;
        start = sep + 1;
    }
    return !out.empty();
}

bool send_key_chord(const KeyChord& chord) {
    if (chord.vk == 0)
        return false;

    std::vector<INPUT> inputs;
    if (chord.ctrl)  append_key(inputs, VK_CONTROL, false);
    if (chord.alt)   append_key(inputs, VK_MENU, false);
    if (chord.shift) append_key(inputs, VK_SHIFT, false);
    if (chord.win)   append_key(inputs, VK_LWIN, false);

    append_key(inputs, chord.vk, false);
    append_key(inputs, chord.vk, true);

    // Release in reverse order so the modifier state unwinds cleanly.
    if (chord.win)   append_key(inputs, VK_LWIN, true);
    if (chord.shift) append_key(inputs, VK_SHIFT, true);
    if (chord.alt)   append_key(inputs, VK_MENU, true);
    if (chord.ctrl)  append_key(inputs, VK_CONTROL, true);

    return dispatch(inputs);
}

bool send_text(const std::string& utf8) {
    const auto wide = widen(utf8);
    if (wide.empty())
        return utf8.empty();

    std::vector<INPUT> inputs;
    inputs.reserve(wide.size() * 2);
    for (wchar_t ch : wide) {
        // Unicode events carry the character directly, so the result does not
        // depend on the target's keyboard layout. Newlines still have to be a
        // real Return, which KEYEVENTF_UNICODE would not deliver.
        if (ch == L'\n') {
            append_key(inputs, VK_RETURN, false);
            append_key(inputs, VK_RETURN, true);
            continue;
        }
        if (ch == L'\r')
            continue;

        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = ch;
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(down);

        INPUT up = down;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        inputs.push_back(up);
    }
    return dispatch(inputs);
}

bool bring_to_foreground(HWND hwnd) {
    if (!IsWindow(hwnd))
        return false;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root)
        root = hwnd;
    if (GetForegroundWindow() == root)
        return true;

    if (IsIconic(root))
        ShowWindow(root, SW_RESTORE);
    SetForegroundWindow(root);

    // SetForegroundWindow is advisory: the shell refuses it when the calling
    // process does not own the foreground. Give the change a moment to land and
    // report whether it actually did, so callers can say so rather than
    // silently clicking the wrong window.
    for (int i = 0; i < 20 && GetForegroundWindow() != root; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return GetForegroundWindow() == root;
}

bool send_click(POINT screenPoint, int button, int clickCount) {
    POINT restore{};
    const bool haveRestore = GetCursorPos(&restore) != FALSE;
    // Put the cursor back even if a SendInput below fails.
    auto restoreCursor = wil::scope_exit([&] {
        if (haveRestore)
            SetCursorPos(restore.x, restore.y);
    });

    if (!SetCursorPos(screenPoint.x, screenPoint.y)) {
        LOG_LAST_ERROR_MSG("SetCursorPos(%ld, %ld)", screenPoint.x, screenPoint.y);
        return false;
    }

    DWORD down = MOUSEEVENTF_LEFTDOWN, up = MOUSEEVENTF_LEFTUP;
    if (button == 1) { down = MOUSEEVENTF_RIGHTDOWN;  up = MOUSEEVENTF_RIGHTUP; }
    if (button == 2) { down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; }

    std::vector<INPUT> inputs;
    for (int i = 0; i < (std::max)(1, clickCount); ++i) {
        INPUT d{};
        d.type = INPUT_MOUSE;
        d.mi.dwFlags = down;
        inputs.push_back(d);
        INPUT u{};
        u.type = INPUT_MOUSE;
        u.mi.dwFlags = up;
        inputs.push_back(u);
    }
    return dispatch(inputs);
}

bool send_wheel(POINT screenPoint, int delta, bool horizontal) {
    POINT restore{};
    const bool haveRestore = GetCursorPos(&restore) != FALSE;
    auto restoreCursor = wil::scope_exit([&] {
        if (haveRestore)
            SetCursorPos(restore.x, restore.y);
    });

    SetCursorPos(screenPoint.x, screenPoint.y);

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta);
    std::vector<INPUT> inputs{input};
    return dispatch(inputs);
}

} // namespace lvt
