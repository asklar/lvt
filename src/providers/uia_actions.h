#pragma once
#include "../element.h"
#include "uia_props.h"
#include "uia_provider.h"

#include <string>
#include <vector>
#include <Windows.h>

namespace lvt {

enum class ActionKind {
    click,        // Invoke, else LegacyIAccessible default action, else a real click
    invoke,       // InvokePattern only
    toggle,       // TogglePattern
    setValue,     // ValuePattern, else RangeValue for numbers, else focus + type
    expand,
    collapse,
    select,       // SelectionItemPattern::Select (replaces the selection)
    addToSelection,
    removeFromSelection,
    selectText,   // TextPattern: find a substring and select that range
    focus,
    scroll,
    typeText,     // send text to the focused element, optionally focusing first
    pressKey,     // key chord(s), optionally focusing an element first
    windowClose,
    windowMinimize,
    windowMaximize,
    windowRestore,
    waitFor,      // block until an element appears, optionally with a property value
    waitGone,     // block until an element disappears
};

struct ActionRequest {
    ActionKind kind = ActionKind::click;

    // Element reference: "eN", a durable key, or "uia:<RuntimeId>". May be empty
    // for typeText/pressKey, which then act on whatever already has focus.
    std::string elementRef;

    std::string text;       // setValue / typeText / pressKey / selectText
    std::string direction;  // scroll: up, down, left, right
    int amount = 1;         // scroll notches, or click count
    int button = 0;         // 0 left, 1 right, 2 middle
    // Right-click and double-click have no pattern equivalent — Invoke is just
    // "activate" — so they must go straight to synthetic input rather than
    // silently succeeding via a pattern that does something else.
    bool forceSyntheticClick = false;

    // waitFor / waitGone. When waitProperty is set, waitFor blocks until the
    // element reports that value rather than merely existing.
    std::string waitProperty;
    std::string waitValue;
    int waitTimeoutMs = 5000;
    int pollIntervalMs = 100;
};

struct ActionResult {
    bool ok = false;
    // How the action was actually carried out — "InvokePattern",
    // "ValuePattern", "SendInput", ... Callers surface this because a pattern
    // and a synthetic click are meaningfully different outcomes.
    std::string method;
    std::string message;
    // The element acted on, after the action, so a caller can see the result
    // without a second walk.
    Element element;
    bool hasElement = false;
};

// Resolve the reference against a fresh UIA walk of the target and perform the
// action. Everything happens on the provider's MTA thread.
//
// Pattern-based paths are preferred throughout: they do not steal focus, do not
// move the cursor, and work when the window is not on top. SendInput is the
// fallback for elements that expose no suitable pattern, and it requires
// bringing the window forward.
ActionResult perform_action(HWND hwnd, const UiaOptions& options,
                            const ActionRequest& request);

// Parse an action name as accepted on the command line.
bool parse_action_kind(const std::string& name, ActionKind& out);
const char* action_kind_name(ActionKind kind);

} // namespace lvt
