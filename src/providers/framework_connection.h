#pragma once
#include "../element.h"
#include <Windows.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lvt {

enum class PropertyEditorKind {
    readonly,
    string,
    boolean,
    integer,
    number,
    enumeration,
};

const char* property_editor_kind_name(PropertyEditorKind kind);
PropertyEditorKind classify_property_editor(
    std::string_view declaredType, bool writable);

struct PropertyChoice {
    std::string value;
    std::string label;
};

// Provider-owned, immutable metadata. Connections cache PropertySchema
// instances and return shared_ptr<const PropertySchema>; values never live in
// the schema, so controls sharing one schema can reuse it safely.
struct PropertyDescriptor {
    std::string descriptorId;
    std::string name;
    std::string displayName;
    std::string provider;
    std::string framework;
    std::string declaringType;
    std::string propertyType;
    PropertyEditorKind kind = PropertyEditorKind::readonly;
    std::vector<PropertyChoice> choices;
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<double> step;
    bool writable = false;
    bool supportsClear = false;
    std::string description;
};

// Per-element live state. runtimeType is deliberately separate from the
// descriptor's declared propertyType: evaluated values can change runtime
// type (or be null), but that must not change editor selection or conversion.
struct PropertyValue {
    std::string descriptorId;
    std::string value;
    std::string runtimeType;
    bool canClear = false;
    bool overridden = false;
    std::string source;
    std::string unavailableReason;
    std::string readOnlyReason;
};

struct PropertySchema {
    std::string schemaId;
    std::vector<PropertyDescriptor> descriptors;
};

struct PropertySnapshotResult {
    bool ok = false;
    HRESULT hresult = E_NOTIMPL;
    std::string error =
        "Typed properties are not supported by this framework connection";
    std::shared_ptr<const PropertySchema> schema;
    std::vector<PropertyValue> values;
};

struct PropertyMutationResult {
    bool ok = false;
    HRESULT hresult = E_NOTIMPL;
    std::string error =
        "Typed property mutation is not supported by this framework connection";
    bool hasValue = false;
    std::string value;
    std::string runtimeType;
    bool canClear = false;
    bool overridden = false;
    std::string source;
    bool cleared = false;
};

// A live, reusable connection to one framework "island" (e.g. one XAML or
// WinUI3 diagnostics session) inside a target process.
//
// This exists so a provider that supports it (today: XamlProvider and
// WinUI3Provider via xaml_diag_common.cpp) can be injected/subscribed ONCE
// and then reused for many subsequent tree refreshes, instead of the old
// per-call model of re-running InitializeXamlDiagnosticsEx from scratch
// every time. That old model is what caused a confirmed, unbounded resource
// leak (one message-only window created and never destroyed per call) and
// the "tree refreshes/resets" bug live-diagnosed against Microsoft Store:
// repeated reconnect/re-advise/re-walk cycles compete for the target's UI
// thread and can occasionally make a whole tick's collection take far
// longer than expected.
//
// A provider that does NOT support this (e.g. Win32Provider/ComCtlProvider,
// which just re-enumerate cheap native HWNDs, or any plugin that hasn't
// implemented the optional plugin ABI v2 connection functions) is simply
// never asked for one — callers fall back to their existing one-shot
// enrich()-per-call path unchanged. See connection_registry.h for how a
// caller (watch's loop, an MCP session) acquires/reuses/releases these.
class IFrameworkConnection {
public:
    virtual ~IFrameworkConnection() = default;

    // Re-walk bounds/properties over the ALREADY established connection and
    // graft the result into `root`. This should be cheap relative to the old
    // one-shot inject_and_collect_xaml_tree: no (re)injection, no fresh
    // AdviseVisualTreeChange, no new message-only window — just a bounds/
    // property refresh dispatched over the connection that is already open.
    // Returns false if the refresh failed (e.g. the pipe broke); callers
    // should treat that the same as is_alive() having gone false and give
    // up on this connection (via the registry) rather than keep retrying it.
    // `providerOption` carries provider-specific filtering/configuration
    // through the generic connection path. Built-in providers ignore it;
    // plugin connections forward it as element_class_filter so persistent
    // and one-shot plugin collection have identical behavior.
    virtual bool get_tree(Element& root, bool fastProperties,
                          const std::string& providerOption = {}) = 0;

    // Non-blocking: returns whatever incremental Add/Remove change
    // notifications have arrived since the last call, without triggering a
    // full tree walk. Empty until the connection's provider implements
    // incremental push (see ConnectionEvent below) - safe to call, and
    // safe to ignore the result, even for a provider that never populates
    // it, since a caller can always fall back to get_tree() for a full
    // refresh.
    virtual std::vector<struct ConnectionEvent> poll_events() = 0;

    // Give a provider with an unsolicited event stream a chance to move bytes
    // already waiting on its transport into the queue returned by
    // poll_events(). The default is a no-op so UIA/plugins remain compatible.
    // XAML uses a lightweight same-pipe acknowledgment rather than rebuilding
    // the tree or opening another diagnostics connection.
    virtual bool refresh_events() {
        return true;
    }

    // Optional provider-neutral typed property operations. XAML and WinUI3
    // implement these over their persistent diagnostics connection. UIA and
    // the other built-in providers retain explicit unsupported defaults until
    // their provider-owned schema adapters are implemented.
    virtual PropertySnapshotResult get_property_snapshot(uint64_t) {
        return {};
    }
    virtual PropertyMutationResult set_property(
        uint64_t, const std::string&, const std::string&) {
        return {};
    }
    virtual PropertyMutationResult clear_property(
        uint64_t, const std::string&) {
        return {};
    }

    // False once the underlying connection is known to be gone (pipe
    // closed/broken, target process exited, etc). A caller holding a
    // reference via the registry should release it once this goes false;
    // the registry will not hand out a dead connection to a new acquirer.
    virtual bool is_alive() const = 0;
};

// One incremental structural change reported by a provider that supports
// push notifications (see IFrameworkConnection::poll_events). Mirrors the
// shape of the Add/Remove mutations XAML diagnostics' own
// IVisualTreeServiceCallback::OnVisualTreeChange already reports — this
// struct is the framework-agnostic version of that, so `watch`'s loop and
// MCP sessions don't need to know which underlying API produced it.
struct ConnectionEvent {
    enum class Mutation { added, removed, snapshotRequired };
    Mutation mutation = Mutation::added;

    // Provider object identity (e.g. XAML InstanceHandle) — the same value
    // Element::providerHandle carries once grafted, so this can be matched
    // directly against an already-built tree without heuristics.
    uint64_t handle = 0;
    uint64_t parentHandle = 0;
    int childIndex = 0;

    // Present for `added`; empty for `removed` (nothing more than the
    // handle is needed to remove a node that already exists in the tree).
    std::string elementType;
    std::string name;
};

} // namespace lvt
