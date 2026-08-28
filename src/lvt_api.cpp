#include "lvt_api.h"

#include "debug.h"
#include "element.h"
#include "element_key.h"
#include "framework_detector.h"
#include "input.h"
#include "json_serializer.h"
#include "lvt_config.h"
#include "screenshot.h"
#include "target.h"
#include "tree_builder.h"
#include "watch_diff.h"
#include "providers/connection_registry.h"

#ifdef LVT_ENABLE_UIA
#include "providers/uia_actions.h"
#include "providers/uia_props.h"
#include "providers/uia_provider.h"
#endif
#if LVT_ENABLE_XAML
#include "providers/xaml_provider.h"
#endif
#if LVT_ENABLE_WINUI3
#include "providers/winui3_provider.h"
#endif

#include <wil/resource.h>
#include <wil/result.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using json = nlohmann::json;

namespace {

std::string to_utf8(const std::wstring& text) {
    if (text.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

// Identify a window well enough for a human or an agent to act on the answer.
// A refusal that says only "another window" leaves the caller guessing between
// "scroll the list" and "that chat window is covering the app".
std::string describe_window(HWND hwnd) {
    std::wstring title;
    const int length = GetWindowTextLengthW(hwnd);
    if (length > 0) {
        title.resize(static_cast<size_t>(length) + 1);
        title.resize(static_cast<size_t>(GetWindowTextW(hwnd, title.data(), length + 1)));
    }

    std::string process;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        wil::unique_handle handle(
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (handle) {
            wchar_t path[MAX_PATH]{};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(handle.get(), 0, path, &size)) {
                const std::wstring full(path, size);
                const auto slash = full.find_last_of(L'\\');
                process = to_utf8(slash == std::wstring::npos ? full : full.substr(slash + 1));
            }
        }
    }

    std::string described = process.empty() ? std::string("another window") : process;
    if (!title.empty())
        described += " (\"" + to_utf8(title) + "\")";
    return described;
}

// --- session registry ---------------------------------------------------
// The MCP server can hold several targets at once, so a connection is a named
// handle rather than an implicit "current" window. Keeping the resolved HWND
// means later calls do not re-run target resolution, which can be ambiguous.

struct Session {
    std::string id;
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::string processName;
    lvt::Architecture architecture = lvt::Architecture::unknown;
    // Which tree this session speaks, and therefore how it acts. UI Automation
    // knows what a control *is*, so it drives through patterns; the visual tree
    // knows where things *are*, so it drives through synthetic input. Keeping
    // them apart means a reference is never resolved against the tree it did
    // not come from.
    bool visualMode = false;
};

std::mutex g_sessionsMutex;
std::map<std::string, Session> g_sessions;
std::atomic<uint64_t> g_nextSession{1};
std::atomic<uint64_t> g_nextScreenshot{1};

enum class SnapshotTree {
    visual,
    uia,
};

struct TreeSnapshotKey {
    std::string session;
    SnapshotTree tree = SnapshotTree::visual;
    std::string consumer;

    bool operator<(const TreeSnapshotKey& other) const {
        return std::tie(session, tree, consumer) <
               std::tie(other.session, other.tree, other.consumer);
    }
};

struct TreeSnapshot {
    lvt::Element tree;
    std::string optionsKey;
};

std::mutex g_treeSnapshotsMutex;
std::map<TreeSnapshotKey, TreeSnapshot> g_treeSnapshots;

std::string add_session(const lvt::TargetInfo& target, bool visualMode) {
    Session session;
    session.id = "s" + std::to_string(g_nextSession.fetch_add(1));
    session.hwnd = target.hwnd;
    session.pid = target.pid;
    session.processName = target.processName;
    session.architecture = target.architecture;
    session.visualMode = visualMode;

    std::lock_guard<std::mutex> lock(g_sessionsMutex);
    g_sessions[session.id] = session;
    return session.id;
}

bool find_session(const std::string& id, Session& out) {
    std::lock_guard<std::mutex> lock(g_sessionsMutex);
    auto it = g_sessions.find(id);
    if (it == g_sessions.end())
        return false;
    out = it->second;
    return true;
}

bool session_is_active(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_sessionsMutex);
    return g_sessions.contains(id);
}

// --- per-session persistent connections ----------------------------------
//
// Session is copied out of g_sessions by find_session/require_session (a
// deliberate choice: methods do their real work without holding
// g_sessionsMutex for that whole time), so a persistent IFrameworkConnection
// - move-only by design, see connection_registry.h's ConnectionHandle -
// cannot live as a Session member; it is kept here instead, keyed by session
// id, and reused across every get_visual_tree/get_uia_tree/find_elements/
// click/... call that session makes, the same way watch's loop reuses one
// across ticks (see main.cpp's acquire_watch_connections). Erased (dropping
// the ConnectionHandles, which disconnect cleanly) in method_disconnect.
std::mutex g_connectionsMutex;
std::map<std::string, std::vector<std::pair<std::string, lvt::ConnectionHandle>>> g_sessionConnections;

// Builds a ConnectionLookup for build_tree, lazily acquiring (once per
// session, on whichever call first needs it) a persistent connection for
// each xaml/winui3 framework this session's target actually has. Returns an
// empty (falsy) ConnectionLookup when the target has neither, so build_tree
// falls back to its normal one-shot-per-call path with no behavior change.
lvt::ConnectionLookup connection_lookup_for_session(const Session& session,
                                                    const std::vector<lvt::FrameworkInfo>& frameworks) {
    bool hasXaml = false, hasWinUI3 = false;
    for (auto& fi : frameworks) {
        if (fi.type == lvt::Framework::Xaml) hasXaml = true;
        if (fi.type == lvt::Framework::WinUI3) hasWinUI3 = true;
    }
    if (!hasXaml && !hasWinUI3)
        return {};

    std::lock_guard<std::mutex> lock(g_connectionsMutex);
    auto& entry = g_sessionConnections[session.id];

    // A connection can die mid-session (a transient timeout against an
    // unusually large/busy tree, or the target recycling something XAML
    // diagnostics-related - see main.cpp's refresh_dead_watch_connections
    // for the live evidence and full reasoning, which applies identically
    // here). Drop any dead entries first so the "what does this session
    // still need" check below is based on current reality, not just
    // whether a label was ever successfully connected once - otherwise a
    // single transient failure would silently and permanently fall back to
    // one-shot-per-call reinjection for the rest of the session.
    for (auto it = entry.begin(); it != entry.end();) {
        if (it->second && !it->second->is_alive()) {
            it->second.reset();
            it = entry.erase(it);
        } else {
            ++it;
        }
    }

    const auto has_label = [&entry](const char* label) {
        for (const auto& [existingLabel, handle] : entry) {
            if (existingLabel == label && handle)
                return true;
        }
        return false;
    };

    const bool needXaml = hasXaml && !has_label("xaml");
    const bool needWinUI3 = hasWinUI3 && !has_label("winui3");
    if (needXaml || needWinUI3) {
        // A full, untrimmed probe tree, needed only to resolve which
        // process/DLL a connection should target (XamlProvider needs to
        // locate the CoreWindow) - discarded once that resolution is done. A
        // session may already hold some other connection here (e.g. UIA, or
        // only one of xaml/winui3 from an earlier partial success), so retry
        // only whichever framework labels are still missing instead of treating
        // "entry is non-empty" as "everything this session could ever need is
        // already connected".
        // Only the native CoreWindow HWND is needed to resolve the process
        // for XAML injection. Do not run the detected framework providers
        // here: that would perform a complete one-shot XAML collection
        // immediately before opening the persistent connection.
        lvt::Element probeTree = lvt::build_tree(session.hwnd, session.pid, {});
#if LVT_ENABLE_XAML
        if (needXaml) {
            auto handle = lvt::ConnectionRegistry::instance().acquire(
                session.pid, session.hwnd, "xaml",
                [&probeTree](HWND hwnd, DWORD pid) -> std::shared_ptr<lvt::IFrameworkConnection> {
                    lvt::XamlProvider xaml;
                    return xaml.open_connection(probeTree, hwnd, pid);
                });
            if (handle)
                entry.emplace_back("xaml", std::move(handle));
        }
#endif
#if LVT_ENABLE_WINUI3
        if (needWinUI3) {
            auto handle = lvt::ConnectionRegistry::instance().acquire(
                session.pid, session.hwnd, "winui3",
                [](HWND hwnd, DWORD pid) -> std::shared_ptr<lvt::IFrameworkConnection> {
                    lvt::WinUI3Provider winui3;
                    return winui3.open_connection(hwnd, pid);
                });
            if (handle)
                entry.emplace_back("winui3", std::move(handle));
        }
#endif
    }

    // Do not return raw pointers into g_sessionConnections. `disconnect`
    // can run concurrently on another MCP worker and erase this entry while
    // build_tree is still using its lookup. A shared snapshot keeps every
    // in-flight connection alive until this synchronous build finishes,
    // independently of the session/registry handle being removed.
    std::vector<std::pair<std::string, std::shared_ptr<lvt::IFrameworkConnection>>> connections;
    connections.reserve(entry.size());
    for (const auto& [label, handle] : entry) {
        if (handle)
            connections.emplace_back(label, handle.shared());
    }
    return [connections = std::move(connections)](
               const std::string& label) -> lvt::IFrameworkConnection* {
        for (const auto& [lbl, connection] : connections) {
            if (lbl == label)
                return connection.get();
        }
        return nullptr;
    };
}

#ifdef LVT_ENABLE_UIA
// UIA mode does not go through build_tree/ConnectionLookup, because the whole
// UIA tree replaces the visual tree rather than enriching it. It still benefits
// from the same "connect once, reuse many times" shape, though: a session can
// keep one client-side IUIAutomation object alive and re-walk through it on
// every request instead of CoCreateInstance + timeout setup on every call.
std::shared_ptr<lvt::UiaConnection> uia_connection_for_session(const Session& session) {
    std::lock_guard<std::mutex> lock(g_connectionsMutex);
    auto& entry = g_sessionConnections[session.id];

    for (auto it = entry.begin(); it != entry.end(); ++it) {
        if (it->first != "uia")
            continue;
        if (it->second && it->second->is_alive())
            return std::dynamic_pointer_cast<lvt::UiaConnection>(it->second.shared());

        it->second.reset();
        entry.erase(it);
        break;
    }

    auto handle = lvt::ConnectionRegistry::instance().acquire(
        session.pid, session.hwnd, "uia",
        [](HWND hwnd, DWORD) -> std::shared_ptr<lvt::IFrameworkConnection> {
            return lvt::UiaConnection::connect(hwnd);
        });
    if (!handle)
        return nullptr;

    auto connection = std::dynamic_pointer_cast<lvt::UiaConnection>(handle.shared());
    entry.emplace_back("uia", std::move(handle));
    return connection;
}
#endif

// --- per-target serialization -------------------------------------------
//
// The MCP server dispatches every request on its own task, so several tool
// calls can be in flight at once — something the CLI, which does one thing and
// exits, never had to survive.
//
// Reading a UI is not actually parallelisable: a UIA walk is a cross-process
// call answered by the target's UI thread, which serves one caller at a time.
// Issuing several at once therefore buys no speed and instead produces
// contention, and under contention the walk does not merely slow down — it
// fails outright with "could not read the UI Automation tree", because the
// connection attempt times out while the target is busy with another walk.
//
// Serializing per target is both correct and free: concurrent requests against
// *different* applications still proceed in parallel, which is the case that
// benefits, while requests against the same application queue instead of
// failing. A caller sees a slower answer rather than a wrong one.
std::mutex g_targetLocksMutex;
std::map<HWND, std::shared_ptr<std::mutex>> g_targetLocks;

std::shared_ptr<std::mutex> lock_for_target(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_targetLocksMutex);
    auto& entry = g_targetLocks[hwnd];
    if (!entry)
        entry = std::make_shared<std::mutex>();
    return entry;
}

// Holds the per-target lock for as long as a walk needs it. Shared ownership
// means a session can be disconnected, and its lock entry dropped, while a walk
// against it is still running.
class TargetGuard {
public:
    explicit TargetGuard(HWND hwnd) : mutex_(lock_for_target(hwnd)), lock_(*mutex_) {}

private:
    std::shared_ptr<std::mutex> mutex_;
    std::lock_guard<std::mutex> lock_;
};

void forget_target_lock(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_targetLocksMutex);
    g_targetLocks.erase(hwnd);
}

// --- helpers ------------------------------------------------------------

std::string get_string(const json& params, const char* key, const char* fallback = "") {
    if (!params.is_object())
        return fallback;
    auto it = params.find(key);
    if (it == params.end() || !it->is_string())
        return fallback;
    return it->get<std::string>();
}

int get_int(const json& params, const char* key, int fallback) {
    if (!params.is_object())
        return fallback;
    auto it = params.find(key);
    if (it == params.end() || !it->is_number_integer())
        return fallback;
    return it->get<int>();
}

bool get_bool(const json& params, const char* key, bool fallback) {
    if (!params.is_object())
        return fallback;
    auto it = params.find(key);
    if (it == params.end() || !it->is_boolean())
        return fallback;
    return it->get<bool>();
}

std::vector<std::string> get_string_array(const json& params, const char* key) {    std::vector<std::string> out;
    if (!params.is_object())
        return out;
    auto it = params.find(key);
    if (it == params.end() || !it->is_array())
        return out;
    for (const auto& entry : *it) {
        if (entry.is_string())
            out.push_back(entry.get<std::string>());
    }
    return out;
}

json element_to_json(const lvt::Element& element, bool includeChildren);

// Attached to any result built from a walk that hit its deadline. Worded for a
// model rather than a developer: the actionable part is that a negative answer
// cannot be trusted, and that raising the timeout is the fix.
const char* truncation_note() {
    return "the UI Automation walk hit its deadline, so this tree is incomplete and "
           "an element may be missing rather than absent; raise timeoutMs and retry "
           "before concluding something is not there";
}

std::vector<unsigned char> read_file_bytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("could not read back the captured screenshot");
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(file),
                                      std::istreambuf_iterator<char>());
}

std::string base64_encode(const std::vector<unsigned char>& bytes) {    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);

    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t triple = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out += kAlphabet[(triple >> 18) & 0x3F];
        out += kAlphabet[(triple >> 12) & 0x3F];
        out += kAlphabet[(triple >> 6) & 0x3F];
        out += kAlphabet[triple & 0x3F];
    }
    if (i < bytes.size()) {
        const bool haveTwo = (i + 1) < bytes.size();
        const uint32_t triple = (bytes[i] << 16) | (haveTwo ? (bytes[i + 1] << 8) : 0);
        out += kAlphabet[(triple >> 18) & 0x3F];
        out += kAlphabet[(triple >> 12) & 0x3F];
        out += haveTwo ? kAlphabet[(triple >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}
// --- element references ---------------------------------------------------
//
// `eN` ids are numbered per tree, so the same control is a different number in
// each: Okta Verify's "Go back" button is e33 in the visual tree and e15 in the
// UIA one. Nothing in a bare `eN` says which tree produced it, which makes a
// reference copied from one tree and used against the other resolve to some
// unrelated element rather than fail.
//
// Every element therefore also carries a qualified `ref` — "uia:e15" or
// "visual:e33" — which is unambiguous and accepted anywhere an element is
// taken. Bare `eN` still works and still means "the tree this tool reads by
// default", so nothing that already worked stops working.

enum class RefTree { unspecified, uia, visual };

struct ParsedRef {
    RefTree tree = RefTree::unspecified;
    std::string ref;  // what to hand to find_element_by_ref
};

bool is_element_id(const std::string& ref) {
    if (ref.size() < 2 || ref[0] != 'e')
        return false;
    return std::all_of(ref.begin() + 1, ref.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

ParsedRef parse_ref(const std::string& ref) {
    // "uia:<RuntimeId>" is the pre-existing form and stays whole, since
    // find_element_by_ref parses it itself. "uia:eN" is the qualified id form.
    if (ref.rfind("uia:", 0) == 0) {
        const auto rest = ref.substr(4);
        if (is_element_id(rest))
            return {RefTree::uia, rest};
        return {RefTree::uia, ref};
    }
    if (ref.rfind("visual:", 0) == 0)
        return {RefTree::visual, ref.substr(7)};

    // Durable keys name the framework that built them, so they are already
    // self-describing: "uia|…" came from the UIA tree, "wpf|…" and friends did
    // not.
    if (ref.rfind("uia|", 0) == 0)
        return {RefTree::uia, ref};
    if (ref.rfind("xaml:0x", 0) == 0 || ref.rfind("winui3:0x", 0) == 0)
        return {RefTree::visual, ref};
    if (ref.find('|') != std::string::npos)
        return {RefTree::visual, ref};

    return {RefTree::unspecified, ref};
}

const char* tree_name(bool uia) { return uia ? "uia" : "visual"; }

// How a visual element relates to the UI Automation tree. `own` is this
// element's own counterpart and is safe to act on; `ancestor` is the
// counterpart of the control it sits inside, which is context rather than a
// target.
struct Correlation {
    std::string own;
    std::string ancestor;
};

using CorrelationMap = std::map<const lvt::Element*, Correlation>;

json element_fields(const lvt::Element& element, const char* tree = nullptr,
                    const CorrelationMap* correlation = nullptr) {
    json j;
    j["id"] = element.id;
    // The unambiguous form, so a reference copied out of one tree cannot be
    // silently resolved against the other.
    if (tree)
        j["ref"] = std::string(tree) + ":" + element.id;
    if (correlation) {
        const auto found = correlation->find(&element);
        if (found != correlation->end()) {
            // Only a counterpart of this element's own is actionable. An
            // ancestor's is reported separately and named for what it is.
            if (!found->second.own.empty())
                j["uiaRef"] = found->second.own;
            else if (!found->second.ancestor.empty())
                j["uiaAncestorRef"] = found->second.ancestor;
        }
    }
    j["key"] = element.key;
    j["type"] = element.type;
    j["framework"] = element.framework;
    if (!element.className.empty())
        j["className"] = element.className;
    if (!element.text.empty())
        j["text"] = element.text;
    j["bounds"] = {{"x", element.bounds.x}, {"y", element.bounds.y},
                   {"width", element.bounds.width}, {"height", element.bounds.height}};
    if (!element.properties.empty())
        j["properties"] = element.properties;
    return j;
}

json element_to_json(const lvt::Element& element, bool includeChildren,
                     const char* tree = nullptr, const CorrelationMap* correlation = nullptr) {
    json j = element_fields(element, tree, correlation);
    if (includeChildren && !element.children.empty()) {
        json children = json::array();
        for (const auto& child : element.children)
            children.push_back(element_to_json(child, true, tree, correlation));
        j["children"] = children;
    }
    return j;
}

void collect_elements(const lvt::Element& element,
                      std::vector<const lvt::Element*>& out) {
    out.push_back(&element);
    for (const auto& child : element.children)
        collect_elements(child, out);
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty())
        return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != haystack.end();
}

bool equals_ci(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

std::string element_property(const lvt::Element& element, const std::string& name) {
    auto builtin = lvt::get_element_property(element, name);
    if (builtin)
        return *builtin;
    auto it = element.properties.find(name);
    return it == element.properties.end() ? std::string() : it->second;
}

#ifdef LVT_ENABLE_UIA
lvt::UiaOptions uia_options_from(const json& params) {
    lvt::UiaOptions options;
    const auto view = get_string(params, "view", "control");
    if (!lvt::parse_uia_view(view, options.view))
        options.view = lvt::UiaView::control;
    options.extraProperties = get_string_array(params, "properties");
    options.timeoutMs = get_int(params, "timeoutMs", 10000);
    return options;
}
#endif

struct InjectedFrameworkShape {
    bool xamlHost = false;
    bool xamlContent = false;
    bool winui3Host = false;
    bool winui3Content = false;
};

void inspect_injected_framework_shape(
    const lvt::Element& element, InjectedFrameworkShape& shape) {
    if (element.className == "Windows.UI.Core.CoreWindow" ||
        element.className == "Windows.UI.Composition.DesktopWindowContentBridge")
        shape.xamlHost = true;
    if (element.className == "Microsoft.UI.Content.DesktopChildSiteBridge" ||
        element.className == "WinUIDesktopWin32WindowClass")
        shape.winui3Host = true;

    const bool isXamlRuntimeType =
        element.className.rfind("Windows.UI.Xaml.", 0) == 0 ||
        element.className.rfind("Microsoft.UI.Xaml.", 0) == 0;
    if (isXamlRuntimeType && element.framework == "xaml")
        shape.xamlContent = true;
    if (isXamlRuntimeType && element.framework == "winui3")
        shape.winui3Content = true;

    for (const auto& child : element.children)
        inspect_injected_framework_shape(child, shape);
}

bool missing_injected_framework_content(const lvt::Element& tree) {
    InjectedFrameworkShape shape;
    inspect_injected_framework_shape(tree, shape);
    return (shape.xamlHost && !shape.xamlContent) ||
           (shape.winui3Host && !shape.winui3Content);
}

const char* injected_framework_for_host(const lvt::Element& element) {
    if (element.className == "Windows.UI.Core.CoreWindow" ||
        element.className == "Windows.UI.Composition.DesktopWindowContentBridge")
        return "xaml";
    if (element.className == "Microsoft.UI.Content.DesktopChildSiteBridge" ||
        element.className == "WinUIDesktopWin32WindowClass")
        return "winui3";
    return nullptr;
}

bool has_framework_descendant(
    const lvt::Element& element, const std::string& framework) {
    for (const auto& child : element.children) {
        if (child.framework == framework &&
            (child.className.rfind("Windows.UI.Xaml.", 0) == 0 ||
             child.className.rfind("Microsoft.UI.Xaml.", 0) == 0))
            return true;
        if (has_framework_descendant(child, framework))
            return true;
    }
    return false;
}

std::string injected_host_identity(const lvt::Element& element) {
    if (element.nativeHandle != 0)
        return element.className + "#" + std::to_string(element.nativeHandle);
    return element.key;
}

void collect_injected_host_state(
    const lvt::Element& element, std::map<std::string, bool>& hosts) {
    if (const char* framework = injected_framework_for_host(element)) {
        hosts[injected_host_identity(element)] =
            has_framework_descendant(element, framework);
    }
    for (const auto& child : element.children)
        collect_injected_host_state(child, hosts);
}

bool lost_populated_injected_host(
    const lvt::Element& previous, const lvt::Element& current) {
    std::map<std::string, bool> previousHosts;
    std::map<std::string, bool> currentHosts;
    collect_injected_host_state(previous, previousHosts);
    collect_injected_host_state(current, currentHosts);
    for (const auto& [identity, hadContent] : previousHosts) {
        if (!hadContent)
            continue;
        auto found = currentHosts.find(identity);
        if (found != currentHosts.end() && !found->second)
            return true;
    }
    return false;
}

// Build the tree a request asked for. `uia` selects the view; the visual tree
// still needs an architecture match because it injects.
//
// `truncated` reports that the UIA walk hit its deadline and the tree is
// incomplete. Passing that back matters more here than it does for the CLI: a
// model that asks "is there a Save button?" and is told "no" will believe it,
// so a partial walk must never be presented as a complete negative answer.
bool build_tree_for(const Session& session, const json& params, bool uia,
                    lvt::Element& tree, std::string& error, bool* truncated = nullptr) {
    if (truncated)
        *truncated = false;
    // One walk of a given window at a time; see the note on g_targetLocks.
    TargetGuard guard(session.hwnd);
    // A request can copy its Session just before a concurrent disconnect
    // removes it, then wait behind disconnect on this target lock. Refuse
    // once it reaches the critical section rather than recreating a
    // connection entry for a session that no longer exists.
    if (!session_is_active(session.id)) {
        error = "this session was disconnected while the request was waiting";
        return false;
    }
    if (uia) {
#ifdef LVT_ENABLE_UIA
        lvt::UiaProvider provider;
        const auto options = uia_options_from(params);

        // Prefer the session's persistent UIA client when one is available, so
        // repeated MCP reads reuse one IUIAutomation object instead of
        // CoCreateInstance + timeout setup on every call. Still retry a failed
        // walk rather than reporting the target unreadable: external readers
        // (screen readers, Inspect.exe, another lvt) can still collide with
        // this process, and a failed/acquisition-reused path should degrade to
        // the exact one-shot walk this code used before the connection work.
        //
        // A UIA walk is answered by the target's UI thread, which serves one
        // caller at a time, so a walk that overlaps another fails its
        // connection attempt instead of merely queueing. The lock above removes
        // the contention this process causes itself, but not the contention
        // from anything else reading the same app — another lvt, a screen
        // reader, Inspect.exe, or a second MCP server. Those are ordinary
        // conditions, not faults, and a transient collision should not be
        // reported to a model as "this window cannot be read".
        std::optional<lvt::Element> result;
        bool wasTruncated = false;
        for (int attempt = 0; attempt < 3 && !result; ++attempt) {
            if (attempt > 0)
                Sleep(static_cast<DWORD>(120 * attempt));

            bool attemptTruncated = false;
            lvt::Element connectedTree;
            if (auto connection = uia_connection_for_session(session)) {
                if (connection->get_tree_with_options(connectedTree, options, &attemptTruncated)) {
                    result = std::move(connectedTree);
                    wasTruncated = attemptTruncated;
                    break;
                }
            }

            result = provider.build(session.hwnd, options, &attemptTruncated);
            wasTruncated = attemptTruncated;
        }
        if (!result) {
            error = "could not read the UI Automation tree for this window; it may be busy "
                    "or not responding";
            return false;
        }
        if (truncated)
            *truncated = wasTruncated;
        tree = std::move(*result);
        lvt::assign_element_ids(tree);
        lvt::assign_element_keys(tree);
        return true;
#else
        error = "this build has UI Automation support compiled out";
        return false;
#endif
    }

    const auto hostArch = lvt::get_host_architecture();
    if (session.architecture != lvt::Architecture::unknown &&
        hostArch != lvt::Architecture::unknown &&
        session.architecture != hostArch) {
        error = std::string("the visual tree cannot be read across architectures: this lvt is ") +
                lvt::architecture_name(hostArch) + " and the target is " +
                lvt::architecture_name(session.architecture) +
                ". Use the UI Automation tree instead, which works across architectures.";
        return false;
    }

    auto frameworks = lvt::detect_frameworks(session.hwnd, session.pid);
    const bool fastProperties = get_bool(params, "fast", false);
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto connectionLookup = connection_lookup_for_session(session, frameworks);
        tree = lvt::build_tree(
            session.hwnd, session.pid, frameworks, -1, {}, fastProperties, connectionLookup);
        if (!missing_injected_framework_content(tree))
            return true;

        // A failed persistent refresh still yields a valid Win32 host
        // skeleton. Treating that as the new truth makes diff consumers
        // remove the entire live XAML subtree, only to add it back after
        // reconnection. Retry so the lookup can prune/reacquire the dead
        // connection, and never advance an MCP snapshot to this transient
        // host-only state.
        if (attempt < 2)
            Sleep(500);
    }

    error =
        "the XAML/WinUI host is still present but its framework tree is temporarily "
        "unavailable; the previous MCP snapshot was preserved";
    return false;
}

// --- methods ------------------------------------------------------------

json method_list_apps(const json& params) {
    const auto nameFilter = get_string(params, "name");
    const auto titleFilter = get_string(params, "title");

    std::vector<lvt::WindowMatch> matches;
    if (!nameFilter.empty())
        matches = lvt::find_by_process_name(nameFilter);
    else if (!titleFilter.empty())
        matches = lvt::find_by_title(titleFilter);
    else
        matches = lvt::find_by_title("");

    json apps = json::array();
    for (const auto& match : matches) {
        char hwnd[32];
        snprintf(hwnd, sizeof(hwnd), "0x%p", static_cast<void*>(match.hwnd));
        apps.push_back({{"hwnd", hwnd},
                        {"pid", static_cast<uint32_t>(match.pid)},
                        {"processName", match.processName},
                        {"title", match.windowTitle}});
    }
    return json{{"apps", apps}};
}

json method_list_sessions(const json&) {
    json sessions = json::array();
    std::lock_guard<std::mutex> lock(g_sessionsMutex);
    for (const auto& [id, session] : g_sessions) {
        sessions.push_back({
            {"session", id},
            {"processName", session.processName},
            {"mode", session.visualMode ? "visual" : "uia"},
        });
    }
    return json{{"sessions", std::move(sessions)}};
}

json method_connect(const json& params) {
    HWND hwnd = nullptr;
    DWORD pid = 0;

    const auto hwndText = get_string(params, "hwnd");
    if (!hwndText.empty())
        hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(strtoull(hwndText.c_str(), nullptr, 0)));
    pid = static_cast<DWORD>(get_int(params, "pid", 0));

    const auto name = get_string(params, "name");
    const auto title = get_string(params, "title");

    if (!hwnd && !pid && !name.empty()) {
        auto matches = lvt::find_by_process_name(name);
        if (matches.empty())
            throw std::runtime_error("no visible windows found for process '" + name + "'");
        if (matches.size() > 1) {
            json options = json::array();
            for (const auto& match : matches) {
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%p", static_cast<void*>(match.hwnd));
                options.push_back({{"hwnd", buf}, {"title", match.windowTitle}});
            }
            // Ambiguity is reported rather than guessed, with the candidates,
            // so the caller can pick instead of acting on the wrong window.
            throw std::runtime_error("several windows match '" + name +
                                     "'; connect by hwnd instead: " + options.dump());
        }
        hwnd = matches[0].hwnd;
    } else if (!hwnd && !pid && !title.empty()) {
        auto matches = lvt::find_by_title(title);
        if (matches.empty())
            throw std::runtime_error("no visible windows found with title containing '" + title + "'");
        if (matches.size() > 1) {
            // Same refusal as the process-name branch above. Title matching is
            // a substring match, so it is the more ambiguous of the two: silently
            // binding a session to an arbitrary one of several windows would
            // send every later call in that session to the wrong place.
            json options = json::array();
            for (const auto& match : matches) {
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%p", static_cast<void*>(match.hwnd));
                options.push_back({{"hwnd", buf},
                                   {"title", match.windowTitle},
                                   {"processName", match.processName}});
            }
            throw std::runtime_error("several windows match the title '" + title +
                                     "'; connect by hwnd instead: " + options.dump());
        }
        hwnd = matches[0].hwnd;
    }

    if (!hwnd && !pid)
        throw std::runtime_error("connect needs one of hwnd, pid, name or title");

    auto target = lvt::resolve_target(hwnd, pid);
    if (!target.hwnd || !IsWindow(target.hwnd))
        throw std::runtime_error("could not resolve a window for that target");

    const auto mode = get_string(params, "mode", "uia");
    if (mode != "uia" && mode != "visual")
        throw std::runtime_error("mode must be 'uia' or 'visual', not '" + mode + "'");
    const bool visualMode = mode == "visual";

    // Driving by geometry means injecting into the target, which needs the same
    // architecture as lvt — the visual tree is built by injecting too.
    if (visualMode) {
        const auto hostArch = lvt::get_host_architecture();
        if (target.architecture != lvt::Architecture::unknown &&
            hostArch != lvt::Architecture::unknown && target.architecture != hostArch) {
            throw std::runtime_error(
                std::string("visual mode needs lvt and the target to share an architecture: this "
                            "lvt is ") +
                lvt::architecture_name(hostArch) + " and the target is " +
                lvt::architecture_name(target.architecture) +
                ". Connect in uia mode instead, which works across architectures.");
        }
    }

    const auto id = add_session(target, visualMode);
    char hwndText2[32];
    snprintf(hwndText2, sizeof(hwndText2), "0x%p", static_cast<void*>(target.hwnd));

    auto frameworks = lvt::detect_frameworks(target.hwnd, target.pid);
    json names = json::array();
    for (const auto& fi : frameworks) {
        auto display = lvt::framework_display_name(fi);
        names.push_back(fi.version.empty() ? display : display + " " + fi.version);
    }

    return json{{"session", id},
                {"hwnd", hwndText2},
                {"pid", static_cast<uint32_t>(target.pid)},
                {"processName", target.processName},
                {"architecture", lvt::architecture_name(target.architecture)},
                {"mode", mode},
                {"frameworks", names}};
}

json method_disconnect(const json& params) {
    const auto id = get_string(params, "session");
    HWND released = nullptr;
    bool stillReferenced = false;
    {
        std::lock_guard<std::mutex> lock(g_sessionsMutex);
        auto it = g_sessions.find(id);
        if (it == g_sessions.end())
            throw std::runtime_error("unknown session '" + id + "'");
        released = it->second.hwnd;
        g_sessions.erase(it);
        // Two sessions may point at the same window, so the lock entry can only
        // go once nothing else needs it.
        for (const auto& [_, session] : g_sessions)
            stillReferenced = stillReferenced || session.hwnd == released;
    }
    {
        // Match the lock order used by tree/action reads: target first,
        // connection map second. This waits for any operation that already
        // entered the target critical section, while those operations hold
        // their own shared connection snapshot so teardown cannot invalidate
        // a raw pointer in flight.
        TargetGuard guard(released);
        // Dropping this session's ConnectionHandles here releases the
        // registry's references (see connection_registry.h); if this was
        // the last reference to a given (pid, framework) connection, its
        // destructor sends a clean DISCONNECT rather than leaving it open
        // until the whole MCP server process eventually exits.
        std::lock_guard<std::mutex> lock(g_connectionsMutex);
        g_sessionConnections.erase(id);
    }
    {
        std::lock_guard<std::mutex> lock(g_treeSnapshotsMutex);
        for (auto it = g_treeSnapshots.begin(); it != g_treeSnapshots.end();) {
            if (it->first.session == id)
                it = g_treeSnapshots.erase(it);
            else
                ++it;
        }
    }
    if (!stillReferenced)
        forget_target_lock(released);
    return json{{"disconnected", id}};
}

Session require_session(const json& params) {
    const auto id = get_string(params, "session");
    Session session;
    if (!find_session(id, session))
        throw std::runtime_error("unknown session '" + id + "'; call connect first");
    if (!IsWindow(session.hwnd))
        throw std::runtime_error("the window for session '" + id + "' has closed");
    return session;
}

// Which tree a tool should read for this session unless told otherwise.
//
// The session's mode is the default, not a hardcoded preference for UI
// Automation. Otherwise a visual session hands out references it will then
// refuse: find_elements would answer with `uia:e6` and click would reject it as
// belonging to the other tree. An explicit `uia` argument still wins, since
// reading the other tree to *understand* an app is reasonable even when you
// drive it through this one.
bool tree_for_session(const Session& session, const json& params) {
    return get_bool(params, "uia", !session.visualMode);
}

#ifdef LVT_ENABLE_UIA
// Identity values a visual element might carry that correspond to a UIA
// AutomationId. Providers spell this differently: XAML and WPF both surface
// x:Name / Name as "name", and XAML additionally keeps automation properties
// under their literal XAML names. Looking only for "AutomationId" — which no
// provider emits — meant the identity path never ran at all, so no real control
// ever correlated and the fallbacks did all the work.
std::vector<std::string> visual_identity_values(const lvt::Element& visual) {
    static constexpr const char* kKeys[] = {
        "AutomationProperties.AutomationId",
        "AutomationId",
        "name",
    };
    std::vector<std::string> values;
    for (const char* key : kKeys) {
        const auto value = element_property(visual, key);
        if (!value.empty() &&
            std::find(values.begin(), values.end(), value) == values.end())
            values.push_back(value);
    }
    return values;
}

// Correlate a whole visual tree against a UIA tree in one pass.
//
// The two trees are not the same nodes numbered differently — they are
// different node sets at different granularities. The WinUI 3 sample has 74
// UIA nodes and 314 visual ones, and a single button is one UIA element but
// three visual ones (Button, its ContentPresenter, its TextBlock). So no
// renumbering could ever make the ids line up; the relationship is many-to-one
// and partial, and the only way to express it is to compute it and say so.
//
// This is reported, never acted on. Actions resolve references only within the
// session's own tree; correlation exists so a caller can see which framework
// elements UI Automation exposes — an element with no counterpart is invisible
// to assistive tech, which is usually a defect in the app being inspected.
void correlate_visual_to_uia(const lvt::Element& uiaTree, const lvt::Element& visualRoot,
                             CorrelationMap& out) {
    std::vector<const lvt::Element*> uiaNodes;
    collect_elements(uiaTree, uiaNodes);

    // AutomationId is unique by contract, so a duplicate means the app has
    // reused one; such an id identifies nothing and is dropped rather than
    // pointed at an arbitrary winner.
    std::map<std::string, const lvt::Element*> byAutomationId;
    std::set<std::string> duplicateIds;
    for (const auto* node : uiaNodes) {
        const auto id = element_property(*node, "AutomationId");
        if (id.empty())
            continue;
        if (!byAutomationId.emplace(id, node).second)
            duplicateIds.insert(id);
    }
    for (const auto& id : duplicateIds)
        byAutomationId.erase(id);

    // Walk depth-first carrying the nearest correlated ancestor.
    //
    // Two different relationships get recorded, and conflating them was a bug:
    //
    //  - `uiaRef` is *this* node's own counterpart, matched by identity. Only
    //    this is safe to act on.
    //  - `uiaAncestorRef` is the counterpart of the control this node sits
    //    inside. Useful context, but not the node itself.
    //
    // Reporting an inherited counterpart as `uiaRef` meant every one of a
    // ListView's 28 item nodes advertised the ListView as the thing to act on,
    // so clicking "item 002" clicked the middle of the whole list and reported
    // success. A template part genuinely has no counterpart of its own; saying
    // so is more useful than pointing at its container and calling it the same
    // thing.
    const std::function<void(const lvt::Element&, const std::string&)> visit =
        [&](const lvt::Element& element, const std::string& inherited) {
            std::string own;
            for (const auto& identity : visual_identity_values(element)) {
                const auto found = byAutomationId.find(identity);
                if (found != byAutomationId.end()) {
                    own = "uia:" + found->second->id;
                    break;
                }
            }
            if (!own.empty())
                out[&element] = Correlation{own, std::string()};
            else if (!inherited.empty())
                out[&element] = Correlation{std::string(), inherited};

            const auto& carry = own.empty() ? inherited : own;
            for (const auto& child : element.children)
                visit(child, carry);
        };
    visit(visualRoot, std::string());
}

#endif

json method_get_tree(const json& params, bool uia) {
    const auto session = require_session(params);
    lvt::Element tree;
    std::string error;
    bool truncated = false;
    if (!build_tree_for(session, params, uia, tree, error, &truncated))
        throw std::runtime_error(error);

    lvt::Element* root = &tree;
    const auto scope = get_string(params, "element");
    if (!scope.empty()) {
        const auto parsed = parse_ref(scope);
        // Scoping to a reference from the *other* tree would silently show a
        // different subtree, so say so instead.
        if ((parsed.tree == RefTree::uia && !uia) || (parsed.tree == RefTree::visual && uia))
            throw std::runtime_error("'" + scope + "' refers to the " +
                                     (parsed.tree == RefTree::uia ? "UI Automation" : "visual") +
                                     " tree, but this is the " + tree_name(uia) + " tree");
        root = lvt::find_element_by_ref(tree, parsed.ref);
        if (!root)
            throw std::runtime_error("element '" + scope + "' not found");
    }

    const int depth = get_int(params, "depth", -1);
    if (depth >= 0)
        lvt::trim_to_depth(*root, depth);

    // Correlating needs the other tree too, so it costs a second walk and is
    // asked for rather than assumed. It answers the question a caller
    // otherwise has to work out for themselves: given this visual element,
    // what do I act on?
    CorrelationMap correlation;
    const bool wantCorrelation = get_bool(params, "correlate", false);
    // Correlation answers "given this visual element, what do I act on?".
    // Asked of the UIA tree it has no meaning — those elements are already the
    // ones you act on. Ignoring the flag would hand back a tree missing the
    // field the caller asked for, with nothing to say why.
    if (wantCorrelation && uia)
        throw std::runtime_error(
            "'correlate' relates visual elements to their UI Automation counterparts, so it "
            "applies to the visual tree only; UI Automation elements are already actionable");
    std::string correlationError;
    bool correlationTruncated = false;
    if (wantCorrelation) {
        lvt::Element uiaTree;
        if (build_tree_for(session, params, true, uiaTree, correlationError,
                           &correlationTruncated))
            // Correlate from the whole tree, not the scoped subtree. The
            // ancestor a node inherits its context from usually sits *above*
            // the scope, so starting at the subtree root threw that away and
            // left a scoped request reporting far less than an unscoped one.
            correlate_visual_to_uia(uiaTree, tree, correlation);
    }

    json out{{"root", element_to_json(*root, true, tree_name(uia),
                                      wantCorrelation ? &correlation : nullptr)}};
    // Naming the tree makes the ids self-describing even for a caller that
    // only reads the top of the response.
    out["tree"] = tree_name(uia);
    if (wantCorrelation) {
        // Count what this response actually reports, not what the whole-tree
        // pass happened to find, or a scoped request would claim credit for
        // correlations the caller cannot see.
        std::vector<const lvt::Element*> reported;
        collect_elements(*root, reported);
        uint64_t inScope = 0;
        for (const auto* element : reported)
            if (correlation.count(element) != 0)
                ++inScope;
        out["correlated"] = inScope;
        // "nothing correlated" and "the UIA side could not be read" look
        // identical from a count alone, and the second is not a statement about
        // the app.
        if (!correlationError.empty())
            out["correlationFailed"] = correlationError;
        else if (correlationTruncated)
            out["correlationPartial"] = truncation_note();
    }
    if (truncated)
        out["truncated"] = truncation_note();
    return out;
}

std::string format_hresult(HRESULT hresult) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "0x%08lX",
             static_cast<unsigned long>(hresult));
    return buffer;
}

struct PropertyTarget {
    std::string provider;
    uint64_t handle = 0;
};

bool parse_compact_property_target(
    const std::string& text, PropertyTarget& out) {
    const auto marker = text.find(":0x");
    if (marker == std::string::npos || marker == 0 || marker + 3 >= text.size())
        return false;
    if (!std::all_of(text.begin(), text.begin() + marker,
                     [](unsigned char ch) {
                         return std::isalnum(ch) != 0 || ch == '_' || ch == '-';
                     })) {
        return false;
    }

    uint64_t handle = 0;
    const char* first = text.data() + marker + 3;
    const char* last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, handle, 16);
    if (parsed.ec != std::errc() || parsed.ptr != last || handle == 0)
        return false;

    out.provider = text.substr(0, marker);
    out.handle = handle;
    return true;
}

PropertyTarget require_property_target(
    const Session& session, const json& params) {
    const auto elementRef = get_string(params, "element");
    if (elementRef.empty())
        throw std::runtime_error("'element' must be a non-empty string");

    const auto parsedRef = parse_ref(elementRef);
    if (parsedRef.tree == RefTree::uia) {
        throw std::runtime_error(
            "'" + elementRef +
            "' is a UI Automation reference, but typed visual properties need "
            "an element from get_visual_tree");
    }

    PropertyTarget target;
    if (parse_compact_property_target(parsedRef.ref, target))
        return target;

    json treeParams = params;
    treeParams["fast"] = true;
    lvt::Element tree;
    std::string error;
    if (!build_tree_for(session, treeParams, false, tree, error))
        throw std::runtime_error(error);
    const auto* element = lvt::find_element_by_ref(tree, parsedRef.ref);
    if (!element)
        throw std::runtime_error("element '" + elementRef + "' not found");
    if (element->framework.empty() || element->providerHandle == 0) {
        throw std::runtime_error(
            "element '" + elementRef +
            "' has no provider-owned property identity");
    }
    target.provider = element->framework;
    target.handle = element->providerHandle;
    return target;
}

lvt::IFrameworkConnection* typed_property_connection(
    const Session& session, const PropertyTarget& target) {
    const auto hostArch = lvt::get_host_architecture();
    if (session.architecture != lvt::Architecture::unknown &&
        hostArch != lvt::Architecture::unknown &&
        session.architecture != hostArch) {
        throw std::runtime_error(
            std::string("native visual properties cannot be read across architectures: this "
                        "lvt is ") +
            lvt::architecture_name(hostArch) + " and the target is " +
            lvt::architecture_name(session.architecture));
    }

    auto frameworks = lvt::detect_frameworks(session.hwnd, session.pid);
    auto lookup = connection_lookup_for_session(session, frameworks);
    auto* connection = lookup ? lookup(target.provider) : nullptr;
    if (!connection || !connection->is_alive()) {
        throw std::runtime_error(
            "provider '" + target.provider +
            "' does not expose a live typed-property connection for this session");
    }
    return connection;
}

json property_snapshot_result(
    const std::string& element, const lvt::PropertySnapshotResult& result) {
    if (!result.ok) {
        throw std::runtime_error(json{
            {"error", result.error.empty() ? "typed property operation failed" : result.error},
            {"hresult", format_hresult(result.hresult)},
        }.dump());
    }

    if (!result.schema)
        throw std::runtime_error("typed property provider returned no schema");

    json out{
        {"ok", true},
        {"element", element},
        {"schemaId", result.schema->schemaId},
        {"descriptors", json::array()},
        {"values", json::array()},
    };
    for (const auto& descriptor : result.schema->descriptors) {
        json item{
            {"descriptorId", descriptor.descriptorId},
            {"name", descriptor.name},
            {"displayName", descriptor.displayName},
            {"provider", descriptor.provider},
            {"framework", descriptor.framework},
            {"declaringType", descriptor.declaringType},
            {"propertyType", descriptor.propertyType},
            {"kind", lvt::property_editor_kind_name(descriptor.kind)},
            {"choices", json::array()},
            {"writable", descriptor.writable},
            {"supportsClear", descriptor.supportsClear},
            {"description", descriptor.description},
        };
        for (const auto& choice : descriptor.choices) {
            item["choices"].push_back({
                {"value", choice.value},
                {"label", choice.label},
            });
        }
        if (descriptor.minimum)
            item["minimum"] = *descriptor.minimum;
        if (descriptor.maximum)
            item["maximum"] = *descriptor.maximum;
        if (descriptor.step)
            item["step"] = *descriptor.step;
        out["descriptors"].push_back(std::move(item));
    }
    for (const auto& value : result.values) {
        out["values"].push_back({
            {"descriptorId", value.descriptorId},
            {"value", value.value},
            {"runtimeType", value.runtimeType},
            {"canClear", value.canClear},
            {"overridden", value.overridden},
            {"source", value.source},
            {"unavailableReason", value.unavailableReason},
            {"readOnlyReason", value.readOnlyReason},
        });
    }
    return out;
}

json property_mutation_result(
    const std::string& element, const std::string& descriptorId,
    const lvt::PropertyMutationResult& result) {
    if (!result.ok) {
        throw std::runtime_error(json{
            {"error", result.error.empty() ? "typed property mutation failed" : result.error},
            {"hresult", format_hresult(result.hresult)},
        }.dump());
    }
    json out{
        {"ok", true},
        {"element", element},
        {"descriptorId", descriptorId},
    };
    if (result.hasValue)
        out["value"] = result.value;
    if (result.cleared)
        out["cleared"] = true;
    return out;
}

json method_get_editable_properties(const json& params) {
    const auto session = require_session(params);
    const auto target = require_property_target(session, params);
    TargetGuard guard(session.hwnd);
    auto* connection = typed_property_connection(session, target);
    return property_snapshot_result(
        get_string(params, "element"),
        connection->get_property_snapshot(target.handle));
}

json method_set_property(const json& params, bool allowInput) {
    if (!allowInput) {
        throw std::runtime_error(
            "'set_property' changes the target application, which needs --allow-input");
    }
    if (params.contains("propertyIndex") || params.contains("valueType")) {
        throw std::runtime_error(
            "set_property accepts only a provider-owned 'descriptorId'; "
            "client-supplied propertyIndex/valueType fields are not allowed");
    }
    const auto session = require_session(params);
    const auto target = require_property_target(session, params);
    const auto descriptorId = get_string(params, "descriptorId");
    if (descriptorId.empty())
        throw std::runtime_error("'descriptorId' must be a non-empty string");
    auto valueIt = params.find("value");
    if (valueIt == params.end() || !valueIt->is_string())
        throw std::runtime_error("'value' must be a string");
    const auto value = valueIt->get<std::string>();

    TargetGuard guard(session.hwnd);
    auto* connection = typed_property_connection(session, target);
    return property_mutation_result(
        get_string(params, "element"), descriptorId,
        connection->set_property(target.handle, descriptorId, value));
}

json method_clear_property(const json& params, bool allowInput) {
    if (!allowInput) {
        throw std::runtime_error(
            "'clear_property' changes the target application, which needs --allow-input");
    }
    if (params.contains("propertyIndex") || params.contains("valueType")) {
        throw std::runtime_error(
            "clear_property accepts only a provider-owned 'descriptorId'; "
            "client-supplied propertyIndex/valueType fields are not allowed");
    }
    const auto session = require_session(params);
    const auto target = require_property_target(session, params);
    const auto descriptorId = get_string(params, "descriptorId");
    if (descriptorId.empty())
        throw std::runtime_error("'descriptorId' must be a non-empty string");

    TargetGuard guard(session.hwnd);
    auto* connection = typed_property_connection(session, target);
    return property_mutation_result(
        get_string(params, "element"), descriptorId,
        connection->clear_property(target.handle, descriptorId));
}

std::string tree_snapshot_options_key(const json& params, bool uia) {
    if (!uia)
        return get_bool(params, "fast", false) ? "fast" : "full";

    json options{
        {"view", get_string(params, "view", "control")},
        {"properties", get_string_array(params, "properties")},
        {"timeoutMs", get_int(params, "timeoutMs", 10000)},
    };
    return options.dump();
}

json method_get_tree_changes(const json& params, bool uia) {
    const auto session = require_session(params);
    lvt::Element current;
    std::string error;
    bool truncated = false;
    if (!build_tree_for(session, params, uia, current, error, &truncated))
        throw std::runtime_error(error);
    if (truncated) {
        throw std::runtime_error(
            "cannot diff a truncated UI Automation tree; increase timeoutMs and try again");
    }

    const TreeSnapshotKey key{
        session.id,
        uia ? SnapshotTree::uia : SnapshotTree::visual,
        get_string(params, "consumer", "tool"),
    };
    const auto optionsKey = tree_snapshot_options_key(params, uia);
    const bool reset = get_bool(params, "reset", false);
    bool snapshot = false;
    std::vector<lvt::ChangeEvent> changes;
    {
        // Keep the session lock through the baseline update. Disconnect removes
        // the session before waiting for an in-flight tree walk, so checking
        // without holding this lock would allow a completed walk to recreate a
        // snapshot after disconnect had already erased it.
        std::lock_guard<std::mutex> sessionsLock(g_sessionsMutex);
        if (!g_sessions.contains(session.id))
            throw std::runtime_error("this session was disconnected while the request was waiting");

        std::lock_guard<std::mutex> snapshotsLock(g_treeSnapshotsMutex);
        auto found = g_treeSnapshots.find(key);
        if (!uia && !reset && found != g_treeSnapshots.end() &&
            found->second.optionsKey == optionsKey &&
            lost_populated_injected_host(found->second.tree, current)) {
            throw std::runtime_error(
                "one XAML/WinUI host temporarily lost its framework subtree; "
                "the previous MCP snapshot was preserved");
        }
        if (reset || found == g_treeSnapshots.end() ||
            found->second.optionsKey != optionsKey) {
            snapshot = true;
            changes = lvt::snapshot_added_events(current);
            g_treeSnapshots[key] = TreeSnapshot{std::move(current), optionsKey};
        } else {
            changes = lvt::diff_trees(found->second.tree, current);
            found->second.tree = std::move(current);
        }
    }

    json events = json::array();
    for (const auto& change : changes) {
        auto event = json::parse(lvt::serialize_change_event(change), nullptr, false);
        if (!event.is_discarded())
            events.push_back(std::move(event));
    }
    return json{{"tree", uia ? "uia" : "visual"},
                {"snapshot", snapshot},
                {"events", std::move(events)}};
}

json method_get_frameworks(const json& params) {
    const auto session = require_session(params);
    auto frameworks = lvt::detect_frameworks(session.hwnd, session.pid);
    json names = json::array();
    for (const auto& fi : frameworks) {
        auto display = lvt::framework_display_name(fi);
        names.push_back(fi.version.empty() ? display : display + " " + fi.version);
    }
    return json{{"frameworks", names}};
}

json method_find_elements(const json& params) {
    const auto session = require_session(params);
    const bool uia = tree_for_session(session, params);
    lvt::Element tree;
    std::string error;
    bool truncated = false;
    if (!build_tree_for(session, params, uia, tree, error, &truncated))
        throw std::runtime_error(error);

    const auto automationId = get_string(params, "automationId");
    const auto name = get_string(params, "name");
    const auto type = get_string(params, "type");
    const auto pattern = get_string(params, "pattern");
    const int limit = get_int(params, "limit", 50);

    // Patterns are a UI Automation concept; the visual tree records how a
    // control is built, not what it can do. Filtering on one here matched
    // nothing and returned an empty list, which reads as "no such control"
    // rather than "this tree cannot answer that".
    if (!pattern.empty() && !uia)
        throw std::runtime_error(
            "'pattern' filters on UI Automation patterns, which the visual tree does not "
            "carry; connect with mode 'uia' to search by pattern, or search this tree by "
            "'type' or 'name'");

    std::vector<const lvt::Element*> all;
    collect_elements(tree, all);

    json matches = json::array();
    for (const auto* element : all) {
        // The visual tree does not carry a property called "AutomationId" — a
        // framework surfaces x:Name/Name as "name", which is what the UIA
        // AutomationId is built from. Checking both spellings means the same
        // query works in either mode, rather than silently finding nothing.
        if (!automationId.empty()) {
            bool matched = element_property(*element, "AutomationId") == automationId;
#ifdef LVT_ENABLE_UIA
            if (!matched) {
                for (const auto& identity : visual_identity_values(*element))
                    matched = matched || identity == automationId;
            }
#endif
            if (!matched)
                continue;
        }
        if (!name.empty() && !contains_ci(element->text, name))
            continue;
        if (!type.empty() && !contains_ci(element->type, type))
            continue;
        if (!pattern.empty() &&
            !contains_ci(element_property(*element, "SupportedPatterns"), pattern))
            continue;
        matches.push_back(element_fields(*element, tree_name(uia)));
        if (static_cast<int>(matches.size()) >= limit)
            break;
    }
    json out{{"elements", matches}, {"searched", static_cast<uint64_t>(all.size())}};
    out["tree"] = tree_name(uia);
    if (truncated)
        out["truncated"] = truncation_note();
    return out;
}

json method_get_element_properties(const json& params) {
    const auto session = require_session(params);
    const auto ref = get_string(params, "element");
    const auto parsed = parse_ref(ref);
    // A qualified reference names its own tree, so it overrides the default
    // rather than being resolved against the wrong one.
    const bool uia = parsed.tree == RefTree::unspecified ? tree_for_session(session, params)
                                                         : parsed.tree == RefTree::uia;

    lvt::Element tree;
    std::string error;
    bool truncated = false;
    if (!build_tree_for(session, params, uia, tree, error, &truncated))
        throw std::runtime_error(error);

    const auto* element = lvt::find_element_by_ref(tree, parsed.ref);
    if (!element) {
        // "Not found" from a partial walk is the false negative this whole
        // mechanism exists to prevent, and this is the tool most likely to be
        // called with a specific id already in hand.
        throw std::runtime_error("element '" + ref + "' not found in the " + tree_name(uia) +
                                 " tree" +
                                 (truncated ? std::string("; note that ") + truncation_note()
                                            : std::string()));
    }

    const auto wanted = get_string_array(params, "properties");
    if (wanted.empty()) {
        json out{{"element", element_fields(*element, tree_name(uia))}};
        out["tree"] = tree_name(uia);
        if (truncated)
            out["truncated"] = truncation_note();
        return out;
    }

    json values = json::object();
    json missing = json::array();
    for (const auto& propertyName : wanted) {
        const auto value = element_property(*element, propertyName);
        if (value.empty())
            missing.push_back(propertyName);
        else
            values[propertyName] = value;
    }
    json out{{"element", element->id}, {"properties", values}};
    out["ref"] = std::string(tree_name(uia)) + ":" + element->id;
    out["tree"] = tree_name(uia);
    // Distinguishing "this element has no such property" from "the value is
    // empty" matters when a caller is probing for pattern support.
    if (!missing.empty())
        out["notPresent"] = missing;
    if (truncated)
        out["truncated"] = truncation_note();
    return out;
}

json method_screenshot(const json& params, bool allowInput) {
    const auto session = require_session(params);
    auto path = get_string(params, "path");

    // Writing to a caller-chosen path creates or truncates that file, which is
    // a side effect outside lvt regardless of the fact that no UI was touched.
    // A server started without --allow-input tells the model it is read-only,
    // so it must not hand out a file-write primitive.
    if (!path.empty() && !allowInput)
        throw std::runtime_error(
            "writing a screenshot to a path needs --allow-input, because it creates or "
            "overwrites a file; omit 'path' to receive the image inline instead");

    // With no path the caller wants the image itself, not a file. Capture to a
    // temp file and hand back base64, cleaning up either way — MCP clients
    // display inline images but cannot read lvt's filesystem.
    const bool inlineImage = path.empty();
    std::filesystem::path tempFile;
    if (inlineImage) {
        wchar_t tempDir[MAX_PATH]{};
        const DWORD written = GetTempPathW(MAX_PATH, tempDir);
        if (written == 0 || written >= MAX_PATH)
            throw std::runtime_error("could not locate a temporary directory for the screenshot");
        tempFile = std::filesystem::path(tempDir) /
                   ("lvt_mcp_" + std::to_string(GetCurrentProcessId()) + "_" +
                    std::to_string(g_nextScreenshot.fetch_add(1)) + ".png");
        path = tempFile.string();
    }
    auto cleanup = wil::scope_exit([&] {
        if (inlineImage) {
            std::error_code ignored;
            std::filesystem::remove(tempFile, ignored);
        }
    });

    lvt::Element tree;
    std::string error;
    // The UIA tree by default, unlike the CLI's screenshot verb.
    //
    // The ids drawn on the image are only useful if they mean the same thing to
    // the tools the caller will act with, and every one of those — find_elements,
    // get_element_properties, hit_test and all twelve action tools — resolves
    // against the UIA tree. The visual tree is a separate `eN` numbering over a
    // different set of nodes, so annotating with it produced ids that silently
    // resolved to unrelated elements: reading `e42` off a screenshot and
    // clicking it activated a list item while reporting success.
    const auto scopeRef = get_string(params, "element");
    const auto parsedScope = parse_ref(scopeRef);
    const bool uia = parsedScope.tree == RefTree::unspecified
                         ? tree_for_session(session, params)
                         : parsedScope.tree == RefTree::uia;
    bool annotated = true;
    bool truncated = false;
    if (!build_tree_for(session, params, uia, tree, error, &truncated)) {
        // Annotation is a bonus; a plain capture is still useful. A requested
        // scope is not a bonus, though — silently returning the whole window
        // would reinterpret the request rather than answer it.
        if (!scopeRef.empty())
            throw std::runtime_error("cannot scope the screenshot to '" + scopeRef +
                                     "' because the tree could not be read: " + error);
        if (lvt::g_debug)
            fprintf(stderr, "lvt: screenshot without annotations: %s\n", error.c_str());
        if (!lvt::capture_screenshot(session.hwnd, path, nullptr, {}))
            throw std::runtime_error("could not capture a screenshot of this window");
        annotated = false;
    } else {
        const auto& scope = parsedScope.ref;
        // An unresolvable scope must not quietly become a full-window capture:
        // a caller who asked to annotate one dialog would get the whole window
        // back with no indication their request was ignored.
        if (!scope.empty() && !lvt::find_element_by_ref(tree, scope))
            throw std::runtime_error("element '" + scopeRef + "' not found in the " +
                                     tree_name(uia) + " tree, so there is nothing "
                                     "to scope the screenshot to");
        if (!lvt::capture_screenshot(session.hwnd, path, &tree, scope))
            throw std::runtime_error("could not capture a screenshot of this window");
    }

    json out{{"annotated", annotated}};
    // Which tree the ids came from, since they are not interchangeable.
    if (annotated)
        out["idsFrom"] = tree_name(uia);
    if (truncated)
        out["truncated"] = truncation_note();
    if (inlineImage)
        out["imageBase64"] = base64_encode(read_file_bytes(path));
    else
        out["path"] = path;
    return out;
}

// Hit-testing is answered from the same tree the caller will address elements
// in, rather than via IUIAutomation::ElementFromPoint. That is deliberate:
// ElementFromPoint can return an element from a different window entirely, and
// one that carries no `eN` id, so the caller could not then act on it. Walking
// our own tree guarantees the answer is both inside the connected app and
// immediately addressable.
json method_hit_test(const json& params) {
    const auto session = require_session(params);
    if (!params.contains("x") || !params.contains("y"))
        throw std::runtime_error("hit_test needs screen coordinates 'x' and 'y'");
    const int x = get_int(params, "x", 0);
    const int y = get_int(params, "y", 0);

    lvt::Element tree;
    std::string error;
    bool truncated = false;
    const bool uia = tree_for_session(session, params);
    if (!build_tree_for(session, params, uia, tree, error, &truncated))
        throw std::runtime_error(error);

    std::vector<const lvt::Element*> all;
    collect_elements(tree, all);

    // The smallest containing element is the most specific one, which is what a
    // click at that point would land on. Ties are broken by tree order.
    const lvt::Element* best = nullptr;
    int64_t bestArea = 0;
    for (const auto* element : all) {
        const auto& b = element->bounds;
        if (b.width <= 0 || b.height <= 0)
            continue;
        if (x < b.x || y < b.y || x >= b.x + b.width || y >= b.y + b.height)
            continue;
        const int64_t area = static_cast<int64_t>(b.width) * b.height;
        if (!best || area < bestArea) {
            best = element;
            bestArea = area;
        }
    }

    if (!best)
        throw std::runtime_error(
            std::string("no element of this window covers the point ") + std::to_string(x) + "," +
            std::to_string(y) +
            (truncated ? std::string("; note that ") + truncation_note() : std::string()));

    json out{{"element", element_fields(*best, tree_name(uia))}};
    out["tree"] = tree_name(uia);
    if (truncated)
        out["truncated"] = truncation_note();
    out["ancestors"] = json::array();
    for (const auto* element : all) {
        const auto& b = element->bounds;
        if (b.width <= 0 || b.height <= 0 || element == best)
            continue;
        if (x >= b.x && y >= b.y && x < b.x + b.width && y < b.y + b.height)
            out["ancestors"].push_back(element->id);
    }
    return out;
}

#ifdef LVT_ENABLE_UIA

// --- keeping the two trees' references apart ------------------------------
//
// Actions in a uia session are carried out through UI Automation, so they need
// a UIA element. An `eN` id or a durable key can just as easily have come from
// get_visual_tree, and the two trees are independent: their `eN` numbering
// covers different nodes, and their durable keys are built from different
// framework paths. Handing a visual reference to the UIA resolver either fails
// outright — "element not found", for a `wpf|…` key with no counterpart — or,
// worse, silently matches a *different* element that happens to occupy the
// same `eN` slot, and acts on that.
//
// lvt used to bridge such a reference: resolve it in the visual tree, then find
// the UIA element it corresponded to by identity, then by text, then by
// position. That is a heuristic making a choice inside an action, where the
// caller cannot see it — and when it chose wrong it clicked something else and
// reported success. Session modes replaced it: a session speaks one tree, hands
// out qualified references from that tree, and refuses the other tree's. This
// is the uia half of that rule; visual_mode_action is the other half.

// Durable keys name the framework that produced them, so they say which tree
// they belong to. `uia|…` is a UIA key; anything else with a path separator is
// a visual one. `eN` and `uia:` refs carry no such marker.
bool looks_like_visual_key(const std::string& ref) {
    if (ref.rfind("uia:", 0) == 0 || ref.rfind("uia|", 0) == 0)
        return false;
    // XAML/WinUI3 use compact diagnostics-handle keys rather than structural
    // paths. They are still visual-tree references and must be rejected by a
    // UIA-mode session before attempting to resolve or act on them.
    if (ref.rfind("xaml:0x", 0) == 0 || ref.rfind("winui3:0x", 0) == 0)
        return true;
    // A durable key always has a framework prefix followed by '|'.
    const auto bar = ref.find('|');
    return bar != std::string::npos && bar > 0;
}


json action_result_to_json(const lvt::ActionResult& result, const std::string& action,
                           const std::string& ref) {
    json out;
    out["action"] = action;
    out["ok"] = result.ok;
    if (!ref.empty())
        out["element"] = ref;
    if (!result.method.empty())
        out["method"] = result.method;
    if (!result.message.empty())
        out["error"] = result.message;
    if (result.hasElement)
        out["result"] = element_fields(result.element, "uia");
    return out;
}

// --- visual mode ---------------------------------------------------------
//
// A session connected in visual mode drives the app the only way the visual
// tree can support: by geometry. It knows where an element is, not what it is,
// so there are no patterns to invoke — a click is a real click at the
// element's centre and typing is real keystrokes.
//
// Nothing here consults UI Automation. That separation is the point: a
// reference is resolved against the tree it came from, so it can never be
// matched to something else.
json visual_mode_action(const Session& session, const json& params, lvt::ActionKind kind,
                        const char* actionName) {
    const auto ref = get_string(params, "element");
    const auto parsed = parse_ref(ref);
    if (parsed.tree == RefTree::uia)
        throw std::runtime_error(
            "'" + ref + "' is a UI Automation reference, but this session is in visual mode. "
            "Connect with mode 'uia' to act through UI Automation patterns, or use a reference "
            "from get_visual_tree.");

    // Classify before doing anything observable. An action this mode cannot
    // express used to be refused only after the window had been restored and
    // raised, so a call that did nothing the caller asked for still rearranged
    // their desktop.
    const bool isWait = kind == lvt::ActionKind::waitFor || kind == lvt::ActionKind::waitGone;
    bool injects = false;
    switch (kind) {
    case lvt::ActionKind::click:
    case lvt::ActionKind::scroll:
    case lvt::ActionKind::typeText:
    case lvt::ActionKind::pressKey:
    case lvt::ActionKind::focus:
        injects = true;
        break;
    case lvt::ActionKind::windowClose:
    case lvt::ActionKind::windowMinimize:
    case lvt::ActionKind::windowMaximize:
    case lvt::ActionKind::windowRestore:
    case lvt::ActionKind::waitFor:
    case lvt::ActionKind::waitGone:
        break;
    default:
        // Toggling, setting a value, expanding, selecting, invoking — these
        // describe what a control *means*, which the visual tree does not know.
        // Saying so is more useful than approximating them with a click that
        // may do something else entirely.
        throw std::runtime_error(
            std::string("'") + actionName +
            "' has no equivalent in visual mode, which drives by geometry rather than by "
            "control semantics. Connect with mode 'uia' to use it, or use click, type, "
            "press_key, scroll or focus here.");
    }

    json out{{"action", actionName}, {"mode", "visual"}};
    if (!ref.empty())
        out["element"] = ref;

    // Waiting observes the visual tree rather than driving anything, so it
    // neither injects nor needs the window in front. It polls the same way the
    // UIA path does, just against the tree this session speaks.
    if (isWait) {
        if (parsed.ref.empty())
            throw std::runtime_error("wait needs an element reference");
        const auto wantPresent = kind == lvt::ActionKind::waitFor;
        const auto property = get_string(params, "waitProperty");
        const auto value = get_string(params, "waitValue");
        const auto deadline =
            GetTickCount64() + static_cast<ULONGLONG>((std::max)(0, get_int(params, "timeoutMs", 5000)));
        for (;;) {
            if (!IsWindow(session.hwnd)) {
                if (!wantPresent) {
                    out["ok"] = true;
                    out["method"] = "window-closed";
                    return out;
                }
                throw std::runtime_error("the window closed while waiting for '" + ref + "'");
            }
            lvt::Element tree;
            std::string error;
            if (build_tree_for(session, params, false, tree, error)) {
                const auto* found = lvt::find_element_by_ref(tree, parsed.ref);
                bool satisfied = wantPresent ? found != nullptr : found == nullptr;
                if (satisfied && wantPresent && found && !property.empty())
                    satisfied = element_property(*found, property) == value;
                if (satisfied) {
                    out["ok"] = true;
                    out["method"] = wantPresent ? "wait-for" : "wait-gone";
                    if (found)
                        out["result"] = element_fields(*found, "visual");
                    return out;
                }
            }
            if (GetTickCount64() >= deadline)
                break;
            Sleep(static_cast<DWORD>((std::max)(10, get_int(params, "pollIntervalMs", 200))));
        }
        throw std::runtime_error(wantPresent
                                     ? "timed out waiting for '" + ref + "'"
                                     : "timed out waiting for '" + ref + "' to disappear");
    }

    // Synthetic input goes to the foreground window, so raising it is a
    // prerequisite rather than a nicety — and it must happen *before* the tree
    // is read, or the bounds captured are the ones the window had while
    // minimized and the first click of a session always misses.
    if (injects && !lvt::bring_to_foreground(session.hwnd))
        throw std::runtime_error("the target window could not be brought to the foreground, so "
                                 "synthetic input would go somewhere else");

    // No TargetGuard here: build_tree_for takes it for the read, and the mutex
    // is not recursive. Injection afterwards is desktop-wide anyway — it goes
    // through the foreground window, not through this target's provider — so
    // holding a per-target lock across it would buy nothing.
    //
    // Restoring a window is not instantaneous, and the framework's cached
    // layout lags further behind: a XAML tree read straight after a restore
    // still reports the -32000 coordinates a minimized window has. Waiting on
    // the window rect is not enough, because that updates first. So the wait is
    // on the thing actually needed — the element being somewhere clickable —
    // and only for an action that is going to aim at it.
    lvt::Element tree;
    std::string error;
    const lvt::Element* element = nullptr;
    const int attempts = injects && !parsed.ref.empty() ? 12 : 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0)
            Sleep(100);
        tree = lvt::Element{};
        if (!build_tree_for(session, params, false, tree, error))
            throw std::runtime_error(error);
        if (parsed.ref.empty())
            break;
        element = lvt::find_element_by_ref(tree, parsed.ref);
        if (!element)
            continue;  // the tree may still be settling; the check below reports it
        const auto& b = element->bounds;
        if (b.width <= 0 || b.height <= 0)
            continue;
        const POINT centre{b.x + b.width / 2, b.y + b.height / 2};
        if (lvt::point_is_on_screen(centre))
            break;
    }

    if (!parsed.ref.empty() && !element)
        throw std::runtime_error("element '" + ref + "' not found in the visual tree");

    const auto requireElement = [&]() -> const lvt::Element& {
        if (!element)
            throw std::runtime_error(std::string(actionName) +
                                     " needs an element reference in visual mode");
        return *element;
    };

    const auto centreOf = [&](const lvt::Element& target) {
        // The provider says when an element is not really showing. WPF and
        // WinForms set "visible", Win32 derives it from IsWindowVisible.
        for (const char* key : {"visible", "IsOffscreen", "winforms.visible"}) {
            const auto value = element_property(target, key);
            if (value.empty())
                continue;
            const bool hidden = equals_ci(key, "IsOffscreen") ? equals_ci(value, "true")
                                                              : equals_ci(value, "false");
            if (hidden)
                throw std::runtime_error("element '" + ref +
                                         "' is not visible, so there is nothing to click");
        }

        const auto& b = target.bounds;
        if (b.width <= 0 || b.height <= 0)
            throw std::runtime_error("element '" + ref +
                                     "' has no on-screen bounds, so there is nothing to aim at");
        const POINT centre{b.x + b.width / 2, b.y + b.height / 2};
        if (!lvt::point_is_on_screen(centre))
            throw std::runtime_error("element '" + ref +
                                     "' is not on any monitor, so it cannot be clicked");

        // Bounds alone do not mean the element is reachable. A realized but
        // scrolled-out list item has perfectly valid on-monitor coordinates
        // outside its viewport, and clicking them delivers the input to
        // whatever is really at that point — measured landing outside the
        // application altogether while reporting success. Asking the system
        // what is actually at the point is the cheap, decisive check.
        HWND atPoint = WindowFromPoint(centre);
        if (atPoint) {
            HWND root = GetAncestor(atPoint, GA_ROOT);
            if (root && root != session.hwnd) {
                // Naming the window that is in the way turns an unactionable
                // refusal into something a caller can respond to: "scroll the
                // list" and "a chat window is sitting on top of the app" need
                // opposite reactions, and the message used to fit both.
                throw std::runtime_error("element '" + ref + "' is at a point covered by " +
                                         describe_window(root) +
                                         " — either it is scrolled out of view or clipped, or "
                                         "that window is on top of the target");
            }
        }
        return centre;
    };

    switch (kind) {
    case lvt::ActionKind::click: {
        const auto centre = centreOf(requireElement());
        const int button = get_int(params, "button", 0);
        if (!lvt::send_click(centre, button, 1))
            throw std::runtime_error("the click could not be delivered");
        out["method"] = "SendInput";
        out["at"] = {{"x", centre.x}, {"y", centre.y}};
        break;
    }

    case lvt::ActionKind::scroll: {
        const auto centre = centreOf(requireElement());
        const auto direction = get_string(params, "direction", "down");
        const int amount = (std::max)(1, get_int(params, "amount", 1));
        const bool horizontal = direction == "left" || direction == "right";
        const bool negative = direction == "down" || direction == "left";
        const int delta = WHEEL_DELTA * amount * (negative ? -1 : 1);
        if (!lvt::send_wheel(centre, delta, horizontal))
            throw std::runtime_error("the scroll could not be delivered");
        out["method"] = "SendInput";
        break;
    }

    case lvt::ActionKind::typeText: {
        // Clicking first puts the caret where the caller pointed; without an
        // element the text goes wherever focus already is.
        if (element) {
            const auto centre = centreOf(*element);
            if (!lvt::send_click(centre, 0, 1))
                throw std::runtime_error("could not click the element to type into it");
        }
        const auto text = get_string(params, "text");
        if (text.empty())
            throw std::runtime_error("type needs some text");
        if (!lvt::send_text(text))
            throw std::runtime_error("the text could not be delivered");
        out["method"] = "SendInput";
        break;
    }

    case lvt::ActionKind::pressKey: {
        if (element) {
            const auto centre = centreOf(*element);
            if (!lvt::send_click(centre, 0, 1))
                throw std::runtime_error("could not click the element to send keys to it");
        }
        std::vector<lvt::KeyChord> chords;
        if (!lvt::parse_key_chords(get_string(params, "text"), chords))
            throw std::runtime_error("could not understand that key chord");
        for (const auto& chord : chords) {
            if (!lvt::send_key_chord(chord))
                throw std::runtime_error("the key chord could not be delivered");
        }
        out["method"] = "SendInput";
        break;
    }

    case lvt::ActionKind::focus: {
        const auto centre = centreOf(requireElement());
        if (!lvt::send_click(centre, 0, 1))
            throw std::runtime_error("could not click the element to focus it");
        out["method"] = "SendInput";
        break;
    }

    case lvt::ActionKind::windowClose:
    case lvt::ActionKind::windowMinimize:
    case lvt::ActionKind::windowMaximize:
    case lvt::ActionKind::windowRestore: {
        // Window commands are Win32, not UI Automation, so they work the same
        // in either mode.
        const int command = kind == lvt::ActionKind::windowMinimize  ? SW_MINIMIZE
                            : kind == lvt::ActionKind::windowMaximize ? SW_MAXIMIZE
                                                                      : SW_RESTORE;
        if (kind == lvt::ActionKind::windowClose)
            PostMessageW(session.hwnd, WM_CLOSE, 0, 0);
        else
            ShowWindow(session.hwnd, command);
        out["method"] = kind == lvt::ActionKind::windowClose ? "WM_CLOSE" : "ShowWindow";
        break;
    }

    default:
        // Unreachable: the classification above already refused anything this
        // mode cannot express, before touching the window.
        throw std::runtime_error(std::string("'") + actionName +
                                 "' has no equivalent in visual mode");
    }

    out["ok"] = true;
    // Synthetic input needed the window on top, which is worth reporting: it
    // changes what the user sees.
    if (injects)
        out["broughtToForeground"] = true;
    return out;
}

json method_action(const json& params, lvt::ActionKind kind, const char* actionName,
                   bool allowInput) {
    const auto session = require_session(params);

    // The waits observe rather than change, so they stay available to a
    // read-only caller; everything else here drives the application.
    const bool isWait = kind == lvt::ActionKind::waitFor || kind == lvt::ActionKind::waitGone;
    if (!isWait && !allowInput)
        throw std::runtime_error(std::string("'") + actionName +
                                 "' changes the target application, which needs --allow-input");

    if (session.visualMode)
        return visual_mode_action(session, params, kind, actionName);

    lvt::ActionRequest request;
    request.kind = kind;
    request.elementRef = get_string(params, "element");
    request.text = get_string(params, "text");
    request.direction = get_string(params, "direction", "down");
    request.amount = get_int(params, "amount", 1);
    request.button = get_int(params, "button", 0);
    request.forceSyntheticClick = get_bool(params, "synthetic", false);
    request.waitProperty = get_string(params, "waitProperty");
    request.waitValue = get_string(params, "waitValue");
    request.waitTimeoutMs = get_int(params, "timeoutMs", 5000);

    // A reference has to belong to the tree this session speaks. "uia:eN" /
    // "visual:eN" and durable keys say which tree they came from; a bare "eN"
    // does not, and is read against the session's default tree.
    //
    // wait-gone is exempt from the check: its success condition is the element
    // being absent, and perform_action already treats an unresolvable reference
    // as satisfied. Refusing first would fail in exactly the case the caller is
    // waiting for, so `close` then `wait_gone` would break.
    const auto originalRef = request.elementRef;
    if (!originalRef.empty() && kind != lvt::ActionKind::waitGone) {
        const auto parsed = parse_ref(originalRef);
        const bool fromVisual = parsed.tree == RefTree::visual ||
                                (parsed.tree == RefTree::unspecified &&
                                 !tree_for_session(session, params));
        // The mirror image of the refusal in visual_mode_action. lvt used to
        // guess a UIA counterpart here; the guess was invisible to the caller
        // and wrong often enough to click the wrong control and report success.
        // Refusing costs one extra call and cannot be wrong.
        if (fromVisual)
            throw std::runtime_error(
                "'" + originalRef +
                "' is a visual-tree reference, but this session is in uia mode. Use a reference "
                "from get_uia_tree or find_elements, or connect with mode 'visual' to drive the "
                "app by geometry. get_visual_tree with correlate:true reports each visual "
                "element's UI Automation counterpart as 'uiaRef' if you need to look one up.");
        request.elementRef = parsed.ref;
    } else if (!originalRef.empty()) {
        // Still strip any qualifier so the reference reaches the resolver in
        // the form it understands.
        request.elementRef = parse_ref(originalRef).ref;
    }

    // perform_action walks the target to resolve the reference, so it needs the
    // same serialization tree reads use. The waits are the exception: they poll
    // until a deadline, and holding the lock for that would stall every other
    // request against the app for the whole timeout — which is precisely when a
    // caller is most likely to also be watching it.
    std::optional<TargetGuard> guard;
    if (!isWait)
        guard.emplace(session.hwnd);
    if (!session_is_active(session.id))
        throw std::runtime_error("this session was disconnected while the request was waiting");

    auto options = uia_options_from(params);
    if (isWait) {
        // For a wait, `timeoutMs` means "how long to keep watching", not "how
        // long a single tree read may take". Letting it drive the UIA
        // transaction timeout as well means a caller who asks to wait two
        // minutes also permits one cross-process call to block for two minutes,
        // which stalls everything else against that app and keeps the process
        // alive long after a client has disconnected. Each poll gets the normal
        // read timeout instead.
        options.timeoutMs = 10000;
    }

    auto connection = uia_connection_for_session(session);
    const auto result = lvt::perform_action(session.hwnd, options, request, connection.get());
    auto out = action_result_to_json(result, actionName, get_string(params, "element"));
    if (!result.ok)
        throw std::runtime_error(out.dump());
    return out;
}
#endif

struct MethodEntry {
    const char* name;
    json (*handler)(const json&);
};

json dispatch(const std::string& method, const json& params, bool allowInput) {
    if (method == "list_apps")   return method_list_apps(params);
    if (method == "list_sessions") return method_list_sessions(params);
    if (method == "connect")     return method_connect(params);
    if (method == "disconnect")  return method_disconnect(params);
    if (method == "get_uia_tree")    return method_get_tree(params, true);
    if (method == "get_visual_tree") return method_get_tree(params, false);
    if (method == "get_uia_tree_changes") return method_get_tree_changes(params, true);
    if (method == "get_visual_tree_changes") return method_get_tree_changes(params, false);
    if (method == "get_editable_properties")
        return method_get_editable_properties(params);
    if (method == "set_property")
        return method_set_property(params, allowInput);
    if (method == "clear_property")
        return method_clear_property(params, allowInput);
    if (method == "get_frameworks")  return method_get_frameworks(params);
    if (method == "find_elements")   return method_find_elements(params);
    if (method == "get_element_properties") return method_get_element_properties(params);
    if (method == "screenshot")  return method_screenshot(params, allowInput);
    if (method == "hit_test")    return method_hit_test(params);

#ifdef LVT_ENABLE_UIA
    if (method == "click")        return method_action(params, lvt::ActionKind::click, "click", allowInput);
    if (method == "invoke")       return method_action(params, lvt::ActionKind::invoke, "invoke", allowInput);
    if (method == "toggle")       return method_action(params, lvt::ActionKind::toggle, "toggle", allowInput);
    if (method == "set_value")    return method_action(params, lvt::ActionKind::setValue, "set-value", allowInput);
    if (method == "expand")       return method_action(params, lvt::ActionKind::expand, "expand", allowInput);
    if (method == "collapse")     return method_action(params, lvt::ActionKind::collapse, "collapse", allowInput);
    if (method == "select")       return method_action(params, lvt::ActionKind::select, "select", allowInput);
    if (method == "add_to_selection")
        return method_action(params, lvt::ActionKind::addToSelection, "add-to-selection", allowInput);
    if (method == "remove_from_selection")
        return method_action(params, lvt::ActionKind::removeFromSelection, "remove-from-selection", allowInput);
    if (method == "select_text")  return method_action(params, lvt::ActionKind::selectText, "select-text", allowInput);
    if (method == "focus")        return method_action(params, lvt::ActionKind::focus, "focus", allowInput);
    if (method == "scroll")       return method_action(params, lvt::ActionKind::scroll, "scroll", allowInput);
    if (method == "type_text")    return method_action(params, lvt::ActionKind::typeText, "type", allowInput);
    if (method == "press_key")    return method_action(params, lvt::ActionKind::pressKey, "press-key", allowInput);
    if (method == "close_window") return method_action(params, lvt::ActionKind::windowClose, "close", allowInput);
    if (method == "minimize_window")
        return method_action(params, lvt::ActionKind::windowMinimize, "minimize", allowInput);
    if (method == "maximize_window")
        return method_action(params, lvt::ActionKind::windowMaximize, "maximize", allowInput);
    if (method == "restore_window")
        return method_action(params, lvt::ActionKind::windowRestore, "restore", allowInput);
    if (method == "wait_for")     return method_action(params, lvt::ActionKind::waitFor, "wait-for", allowInput);
    if (method == "wait_gone")    return method_action(params, lvt::ActionKind::waitGone, "wait-gone", allowInput);
#endif

    throw std::runtime_error("unknown method '" + method + "'");
}

char* duplicate(const std::string& text) {
    // Allocated with malloc so lvt_api_free can release it with free, keeping
    // every allocation on lvt's own heap.
    auto* copy = static_cast<char*>(malloc(text.size() + 1));
    if (!copy)
        return nullptr;
    memcpy(copy, text.c_str(), text.size() + 1);
    return copy;
}

} // namespace

extern "C" int32_t lvt_api_call(const char* method, const char* params_json,
                                int32_t allow_input, char** result_json) {
    if (!result_json)
        return -1;
    *result_json = nullptr;
    if (!method)
        return -1;

    std::string payload;
    int32_t status = 0;
    try {
        json params = json::object();
        if (params_json && *params_json) {
            params = json::parse(params_json, nullptr, false);
            if (params.is_discarded())
                throw std::runtime_error("params is not valid JSON");
        }
        payload = dispatch(method, params, allow_input != 0).dump();
    } catch (const std::exception& ex) {
        // Errors come back as JSON too, so the caller always has something
        // structured to surface rather than a bare status code.
        json error;
        const std::string what = ex.what();
        auto parsed = json::parse(what, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object())
            error = parsed;
        else
            error = json{{"error", what}};
        error["ok"] = false;
        payload = error.dump();
        status = 1;
    } catch (...) {
        payload = json{{"ok", false}, {"error", "unknown failure"}}.dump();
        status = 1;
    }

    *result_json = duplicate(payload);
    return *result_json ? status : -1;
}

extern "C" void lvt_api_free(char* result_json) {
    free(result_json);
}

extern "C" const char* lvt_api_version(void) {
    return LVT_VERSION;
}
