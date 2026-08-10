#pragma once
#include "provider.h"
#include "uia_props.h"

#include <optional>
#include <string>
#include <vector>

namespace lvt {

struct UiaOptions {
    UiaView view = UiaView::control;

    // Extra properties beyond the core set, by the names in uia_props.h.
    // Unknown names are reported and ignored rather than failing the walk.
    std::vector<std::string> extraProperties;

    // Walk deadline in milliseconds, defaulting to a value comfortably above a
    // real app's walk (the heaviest measured — a WebView2 host with ~380
    // elements — takes about 1.3s) while staying well under UIA's own 20s
    // default, so a wedged target fails in a timeframe a caller will tolerate.
    //
    // It bounds two distinct things: how long UIA waits for any single provider
    // response (its transaction timeout), and the traversal of the materialised
    // cache. It is not a hard cap on total wall time, since one walk can involve
    // many provider responses.
    //
    // 0 removes the lvt-imposed deadline; UIA's own default still applies.
    int timeoutMs = 10000;
};

// Walks the target's UI Automation tree and returns it as a standard
// lvt::Element tree, so ids, durable keys, --element/--query scoping, --watch
// diffing and screenshot annotation all work on it unchanged.
//
// Unlike the visual-tree providers this injects nothing into the target, so it
// works cross-architecture and against processes lvt has no provider for.
//
// All work happens on a dedicated MTA thread: UIA clients want an MTA, while
// screenshot.cpp initializes an STA on the calling thread, and mixing them on
// one thread yields RPC_E_CHANGED_MODE.
class UiaProvider : public IProvider {
public:
    // Returns std::nullopt if the UIA client could not be created, the window
    // has no UIA element, or the cache request exceeded the timeout.
    //
    // When the deadline cuts the traversal short, `truncated` is set and the
    // returned root carries a "Truncated" property, so a consumer reading only
    // the document can still tell the tree is incomplete.
    std::optional<Element> build(HWND hwnd, const UiaOptions& options, bool* truncated = nullptr);
};

// Format a UIA RuntimeId as the dotted string lvt emits, e.g. "42.1234.0".
std::string format_runtime_id(const std::vector<int>& runtimeId);

// Parse the dotted RuntimeId form back to its components. Returns false if the
// string is not a well-formed RuntimeId.
bool parse_runtime_id(const std::string& text, std::vector<int>& out);

} // namespace lvt
