#pragma once
#include "element.h"
#include "framework_detector.h"
#include "providers/framework_connection.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lvt {

// Looks up an already-established, reusable connection for a given
// framework label ("xaml"/"winui3"), instead of build_tree re-injecting a
// fresh one-shot connection every call. Returns nullptr (or is left unset
// entirely) to keep today's one-shot-per-call behavior - this is how a
// one-shot CLI command (dump/query/screenshot) still works unchanged; only
// a caller that holds a connection across many build_tree calls (watch's
// loop, an MCP session - see connection_registry.h) supplies one.
//
// Returns a raw pointer, not a shared_ptr: the callback is only ever used
// synchronously within one build_tree call, and the caller supplying it
// already holds the real, refcounted ownership via a ConnectionHandle for
// as long as its own loop/session runs - returning a shared_ptr here would
// invite a stray copy to outlive that handle and the registry's own
// bookkeeping, which is exactly the kind of "forgot to release" bug this
// whole mechanism exists to avoid.
using ConnectionLookup = std::function<IFrameworkConnection*(const std::string& frameworkLabel)>;

// Build a unified visual tree from the given HWND using detected frameworks.
// `fastProperties` skips IVisualTreeService::GetPropertyValuesChain for
// XAML/WinUI3 elements (the dominant per-element cost of a rich tree) in
// favor of cheaper direct WinRT property reads — see
// xaml_diag_common.h's inject_and_collect_xaml_tree for exactly what that
// trades away. Defaults to false (today's exhaustive property collection),
// unaffected for every non-XAML/WinUI3 provider.
Element build_tree(HWND hwnd, DWORD pid, const std::vector<FrameworkInfo>& frameworks,
                   int maxDepth = -1, const std::string& pluginOption = {},
                   bool fastProperties = false,
                   const ConnectionLookup& connectionLookup = {});

// Assign deterministic element IDs (e0, e1, ...) in depth-first order.
void assign_element_ids(Element& root);

// Find an element by its assigned positional ID, durable key, or either (ID first).
Element* find_element_by_id(Element& root, const std::string& id);
const Element* find_element_by_id(const Element& root, const std::string& id);
Element* find_element_by_key(Element& root, const std::string& key);
const Element* find_element_by_key(const Element& root, const std::string& key);

// Resolve an element reference: a positional id ("e4"), a durable key, or a
// "uia:<RuntimeId>" reference against a UIA tree.
Element* find_element_by_ref(Element& root, const std::string& ref);
const Element* find_element_by_ref(const Element& root, const std::string& ref);

// Return a built-in or dynamic element property value.
std::optional<std::string> get_element_property(const Element& element, const std::string& property);

// Trim element tree to a maximum depth (0 = root only, 1 = root + children, etc.)
void trim_to_depth(Element& root, int maxDepth);

} // namespace lvt
