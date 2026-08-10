#pragma once
#include <string>
#include <vector>
#include <Windows.h>

namespace lvt {

// Synthetic input, used as the fallback when a UI Automation pattern cannot
// perform an action. These steal focus and move the cursor, so prefer the
// pattern-based path in uia_actions.h wherever the element supports one.

struct KeyChord {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool win = false;
    WORD vk = 0;
};

// Parse "Ctrl+Shift+S", "Enter", "F5", "Alt+F4". Modifier and key names are
// case-insensitive. Returns false for an empty chord, an unknown key name, or
// modifiers with no key.
bool parse_key_chord(const std::string& text, KeyChord& out);

// Parse a semicolon- or comma-separated sequence: "Ctrl+A;Delete".
bool parse_key_chords(const std::string& text, std::vector<KeyChord>& out);

// Press and release a chord, holding modifiers for the duration of the key.
bool send_key_chord(const KeyChord& chord);

// Type UTF-8 text as Unicode key events, so it does not depend on the current
// keyboard layout. Goes to whatever currently has focus.
bool send_text(const std::string& utf8);

// Click at a screen point. Saves and restores the cursor position, and brings
// the owning window forward first, because synthetic clicks land on whatever is
// actually on top.
bool send_click(POINT screenPoint, int button = 0, int clickCount = 1);

// Wheel scroll at a screen point. Positive delta scrolls up / right.
bool send_wheel(POINT screenPoint, int delta, bool horizontal = false);

// Bring the window containing an element to the foreground. Synthetic input is
// delivered to the foreground window, so this is a prerequisite for the
// SendInput fallbacks rather than an action in its own right.
bool bring_to_foreground(HWND hwnd);

} // namespace lvt
