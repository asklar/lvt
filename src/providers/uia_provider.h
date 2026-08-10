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

    // Deadline for the whole walk. UIA calls cross process boundaries and can
    // block indefinitely on an unresponsive target, so the walk is bounded and
    // returns what it has rather than hanging. 0 disables the deadline.
    int timeoutMs = 10000;

    int maxDepth = -1;
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
    // Returns std::nullopt if the UIA client could not be created or the window
    // has no UIA element. `truncated` is set when the deadline cut the walk short.
    std::optional<Element> build(HWND hwnd, const UiaOptions& options, bool* truncated = nullptr);
};

// Format a UIA RuntimeId as the dotted string lvt emits, e.g. "42.1234.0".
std::string format_runtime_id(const std::vector<int>& runtimeId);

// Parse the dotted RuntimeId form back to its components. Returns false if the
// string is not a well-formed RuntimeId.
bool parse_runtime_id(const std::string& text, std::vector<int>& out);

} // namespace lvt
