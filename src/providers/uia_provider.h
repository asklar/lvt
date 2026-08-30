#pragma once
#include "framework_connection.h"
#include "provider.h"
#include "uia_props.h"
#include "../target.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct IUIAutomation;
struct IUIAutomationElement;

namespace lvt {

class UiaWindowLifetimeToken;

struct UiaTargetIdentity {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    uint64_t processCreationIdentity = 0;
    std::vector<int> rootRuntimeId;
    std::shared_ptr<UiaWindowLifetimeToken> windowLifetime;

    bool valid() const {
        return hwnd && pid && processCreationIdentity &&
               !rootRuntimeId.empty() && windowLifetime;
    }
};

std::optional<UiaTargetIdentity> capture_uia_target_identity(
    HWND hwnd, DWORD expectedPid,
    uint64_t expectedProcessCreationIdentity = 0);
HRESULT validate_uia_target_identity(
    const UiaTargetIdentity& identity);

HRESULT get_validated_uia_root(
    IUIAutomation* automation,
    const UiaTargetIdentity& identity,
    HANDLE retainedProcess,
    IUIAutomationElement** root,
    const char* testGateEnvironment = nullptr);

namespace uia_eventing_detail {

class SnapshotHint {
public:
    void signal() noexcept {
        uint32_t state = m_state.load(std::memory_order_acquire);
        while ((state & kAccepting) != 0 &&
               (state & kSnapshotRequired) == 0 &&
               !m_state.compare_exchange_weak(
                   state, state | kSnapshotRequired,
                   std::memory_order_release, std::memory_order_acquire)) {
        }
    }

    bool consume() noexcept {
        uint32_t state = m_state.load(std::memory_order_acquire);
        while ((state & kSnapshotRequired) != 0) {
            if (m_state.compare_exchange_weak(
                    state, state & ~kSnapshotRequired,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void stop() noexcept {
        m_state.store(0, std::memory_order_release);
    }

private:
    static constexpr uint32_t kAccepting = 1;
    static constexpr uint32_t kSnapshotRequired = 2;
    std::atomic<uint32_t> m_state{kAccepting};
};

struct SubscriptionCounters {
    uint64_t connections = 0;
    uint64_t structureRegistrations = 0;
    uint64_t propertyRegistrations = 0;
    uint64_t automationRegistrations = 0;
    uint64_t callbacks = 0;
    uint64_t removeAllCalls = 0;
};

SubscriptionCounters subscription_counters();
void reset_subscription_counters();
const std::vector<int>& subscribed_property_ids();
const std::vector<int>& subscribed_automation_event_ids();

} // namespace uia_eventing_detail

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

// Identity namespaces differ only when the UIA tree filter changes. Requested
// properties and timeout policy affect how a snapshot is collected, not which
// elements its RuntimeIds and durable keys identify.
std::string uia_identity_scope(const UiaOptions& options);

// Session-local identity adapter used both for persistent UIA walks and for
// one-shot fallback trees. It stores only RuntimeIds/keys and opaque numeric
// handles—never live values or COM objects.
class UiaPropertyIdentityCache {
public:
    static constexpr size_t kMaximumRuntimeIds = 16384;
    static constexpr size_t kMaximumKeyAliases = 32768;
    static constexpr size_t kMaximumScopes = 16;

    bool attach(
        Element& root, const std::string& scope = "default",
        bool completeSnapshot = true);
    bool remember(
        const Element& root, const std::string& scope = "default",
        bool completeSnapshot = true);
    std::optional<uint64_t> resolve(
        const std::string& reference, std::string& error);
    std::optional<std::string> runtime_id(uint64_t handle) const;
    size_t runtime_id_count() const { return m_handlesByRuntimeId.size(); }
    size_t key_alias_count() const;
    size_t scope_count() const { return m_scopes.size(); }

private:
    using KeyAliases = std::unordered_map<
        std::string, std::unordered_set<std::string>>;

    struct RuntimeIdentity {
        uint64_t handle = 0;
        uint64_t lastUsed = 0;
        std::unordered_map<std::string, uint64_t> lastSeenByScope;
    };
    struct ScopeState {
        uint64_t generation = 0;
        uint64_t lastUsed = 0;
    };

    void attach_element(Element& element);
    void remember_element(const Element& element);
    void collect_runtime_ids(
        const Element& element,
        std::unordered_set<std::string>& runtimeIds) const;
    void collect_key_aliases(
        const Element& element,
        KeyAliases& aliases) const;
    std::optional<std::string> scope_to_evict(
        const std::string& incomingScope) const;
    void evict_scope(const std::string& scope);
    void prune(
        const std::unordered_set<std::string>& protectedRuntimeIds = {},
        const KeyAliases& protectedAliases = {});

    uint64_t m_nextHandle = 1;
    uint64_t m_clock = 0;
    std::string m_activeScope = "default";
    uint64_t m_activeGeneration = 0;
    std::unordered_map<std::string, ScopeState> m_scopes;
    std::unordered_map<
        std::string, std::unordered_set<std::string>>
        m_currentRuntimeIdsByScope;
    std::unordered_map<
        std::string, KeyAliases>
        m_currentAliasesByScope;
    std::unordered_map<std::string, RuntimeIdentity> m_handlesByRuntimeId;
    std::unordered_map<uint64_t, std::string> m_runtimeIdsByHandle;
    std::unordered_map<
        std::string, std::unordered_map<std::string, uint64_t>> m_runtimeIdsByKey;
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
    std::optional<Element> build(
        const UiaTargetIdentity& identity,
        const UiaOptions& options,
        bool* truncated = nullptr,
        bool* ownershipLost = nullptr);
};

// Reusable UIA client for callers that read the same target repeatedly (watch,
// MCP sessions). Unlike the visual-tree connections this never injects into the
// target; it amortizes CoCreateInstance(CUIAutomation[8]) across many walks,
// owns root-scoped event handlers, and validates every use against the
// resolver-supplied expected PID.
class UiaConnection : public IFrameworkConnection {
public:
    static std::shared_ptr<UiaConnection> connect(
        const UiaTargetIdentity& identity);
    static std::shared_ptr<UiaConnection> connect(
        HWND hwnd, DWORD expectedPid,
        uint64_t expectedProcessCreationIdentity = 0);
    ~UiaConnection() override;

    bool get_tree(Element& root, bool fastProperties,
                  const std::string& providerOption = {}) override;
    bool get_tree_with_options(Element& root, const UiaOptions& options,
                               bool* truncated = nullptr);
    bool attach_property_identities(
        Element& root, const UiaOptions& options,
        bool completeSnapshot = true);
    bool remember_property_references(
        const Element& root, const UiaOptions& options,
        bool completeSnapshot = true);
    std::string property_identity_error();
    std::optional<uint64_t> resolve_property_reference(
        const std::string& reference, std::string& error);
    HWND target_hwnd() const { return m_hwnd; }
    const UiaTargetIdentity& target_identity() const {
        return m_identity;
    }
    bool matches_target(HWND hwnd) const;
    PropertySnapshotResult get_property_snapshot(uint64_t handle) override;
    PropertyMutationResult set_property(
        uint64_t handle, const std::string& descriptorId,
        const std::string& value) override;
    PropertyMutationResult clear_property(
        uint64_t handle, const std::string& descriptorId) override;
    bool refresh_events() override;
    std::vector<ConnectionEvent> poll_events() override;
    bool is_alive() const override;
    void fail_next_tree_for_testing();

private:
    explicit UiaConnection(UiaTargetIdentity identity);
    bool validate_target_identity_locked() const;

    struct State;

    HWND m_hwnd = nullptr;
    DWORD m_pid = 0;
    UiaTargetIdentity m_identity;
    std::unique_ptr<State> m_state;
};

// Round-trip-safe wire formatting for UIA double properties and RangeValue
// readback. Parsing the result with strtod reproduces the original double.
std::string format_uia_double(double value);

// Format a UIA RuntimeId as the dotted string lvt emits, e.g. "42.1234.0".
std::string format_runtime_id(const std::vector<int>& runtimeId);

// Parse the dotted RuntimeId form back to its components. Returns false if the
// string is not a well-formed RuntimeId.
bool parse_runtime_id(const std::string& text, std::vector<int>& out);

} // namespace lvt
