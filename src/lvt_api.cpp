#include "lvt_api.h"

#include "debug.h"
#include "element.h"
#include "element_key.h"
#include "framework_detector.h"
#include "json_serializer.h"
#include "lvt_config.h"
#include "screenshot.h"
#include "target.h"
#include "tree_builder.h"

#ifdef LVT_ENABLE_UIA
#include "providers/uia_actions.h"
#include "providers/uia_props.h"
#include "providers/uia_provider.h"
#endif

#include <wil/resource.h>
#include <wil/result.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

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
};

std::mutex g_sessionsMutex;
std::map<std::string, Session> g_sessions;
std::atomic<uint64_t> g_nextSession{1};
std::atomic<uint64_t> g_nextScreenshot{1};

std::string add_session(const lvt::TargetInfo& target) {
    Session session;
    session.id = "s" + std::to_string(g_nextSession.fetch_add(1));
    session.hwnd = target.hwnd;
    session.pid = target.pid;
    session.processName = target.processName;
    session.architecture = target.architecture;

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
json element_fields(const lvt::Element& element) {
    json j;
    j["id"] = element.id;
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

json element_to_json(const lvt::Element& element, bool includeChildren) {
    json j = element_fields(element);
    if (includeChildren && !element.children.empty()) {
        json children = json::array();
        for (const auto& child : element.children)
            children.push_back(element_to_json(child, true));
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
    if (uia) {
#ifdef LVT_ENABLE_UIA
        lvt::UiaProvider provider;
        const auto options = uia_options_from(params);

        // Retry a failed walk rather than reporting the target unreadable.
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
            result = provider.build(session.hwnd, options, &wasTruncated);
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
    tree = lvt::build_tree(session.hwnd, session.pid, frameworks, -1, {});
    return true;
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
        hwnd = matches[0].hwnd;
    }

    if (!hwnd && !pid)
        throw std::runtime_error("connect needs one of hwnd, pid, name or title");

    auto target = lvt::resolve_target(hwnd, pid);
    if (!target.hwnd || !IsWindow(target.hwnd))
        throw std::runtime_error("could not resolve a window for that target");

    const auto id = add_session(target);
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
        root = lvt::find_element_by_ref(tree, scope);
        if (!root)
            throw std::runtime_error("element '" + scope + "' not found");
    }

    const int depth = get_int(params, "depth", -1);
    if (depth >= 0)
        lvt::trim_to_depth(*root, depth);

    json out{{"root", element_to_json(*root, true)}};
    if (truncated)
        out["truncated"] = truncation_note();
    return out;
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
    const bool uia = get_bool(params, "uia", true);
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

    std::vector<const lvt::Element*> all;
    collect_elements(tree, all);

    json matches = json::array();
    for (const auto* element : all) {
        if (!automationId.empty() && element_property(*element, "AutomationId") != automationId)
            continue;
        if (!name.empty() && !contains_ci(element->text, name))
            continue;
        if (!type.empty() && !contains_ci(element->type, type))
            continue;
        if (!pattern.empty() &&
            !contains_ci(element_property(*element, "SupportedPatterns"), pattern))
            continue;
        matches.push_back(element_fields(*element));
        if (static_cast<int>(matches.size()) >= limit)
            break;
    }
    json out{{"elements", matches}, {"searched", static_cast<uint64_t>(all.size())}};
    if (truncated)
        out["truncated"] = truncation_note();
    return out;
}

json method_get_element_properties(const json& params) {
    const auto session = require_session(params);
    const bool uia = get_bool(params, "uia", true);
    lvt::Element tree;
    std::string error;
    if (!build_tree_for(session, params, uia, tree, error))
        throw std::runtime_error(error);

    const auto ref = get_string(params, "element");
    const auto* element = lvt::find_element_by_ref(tree, ref);
    if (!element)
        throw std::runtime_error("element '" + ref + "' not found");

    const auto wanted = get_string_array(params, "properties");
    if (wanted.empty())
        return json{{"element", element_fields(*element)}};

    json values = json::object();
    for (const auto& propertyName : wanted) {
        const auto value = element_property(*element, propertyName);
        if (!value.empty())
            values[propertyName] = value;
    }
    return json{{"element", element->id}, {"properties", values}};
}

json method_screenshot(const json& params) {
    const auto session = require_session(params);
    auto path = get_string(params, "path");

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
    const bool uia = get_bool(params, "uia", false);
    bool annotated = true;
    if (!build_tree_for(session, params, uia, tree, error)) {
        // Annotation is a bonus; a plain capture is still useful.
        if (lvt::g_debug)
            fprintf(stderr, "lvt: screenshot without annotations: %s\n", error.c_str());
        if (!lvt::capture_screenshot(session.hwnd, path, nullptr, {}))
            throw std::runtime_error("could not capture a screenshot of this window");
        annotated = false;
    } else {
        const auto scope = get_string(params, "element");
        if (!lvt::capture_screenshot(session.hwnd, path, &tree, scope))
            throw std::runtime_error("could not capture a screenshot of this window");
    }

    json out{{"annotated", annotated}};
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
    if (!build_tree_for(session, params, get_bool(params, "uia", true), tree, error, &truncated))
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

    json out{{"element", element_fields(*best)}};
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
        out["result"] = element_fields(result.element);
    return out;
}

json method_action(const json& params, lvt::ActionKind kind, const char* actionName) {
    const auto session = require_session(params);

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

    // perform_action walks the target to resolve the reference, so it needs the
    // same serialization tree reads use. The waits are the exception: they poll
    // until a deadline, and holding the lock for that would stall every other
    // request against the app for the whole timeout — which is precisely when a
    // caller is most likely to also be watching it.
    const bool isWait = kind == lvt::ActionKind::waitFor || kind == lvt::ActionKind::waitGone;
    std::optional<TargetGuard> guard;
    if (!isWait)
        guard.emplace(session.hwnd);

    const auto result = lvt::perform_action(session.hwnd, uia_options_from(params), request);
    auto out = action_result_to_json(result, actionName, request.elementRef);
    if (!result.ok)
        throw std::runtime_error(out.dump());
    return out;
}
#endif

struct MethodEntry {
    const char* name;
    json (*handler)(const json&);
};

json dispatch(const std::string& method, const json& params) {
    if (method == "list_apps")   return method_list_apps(params);
    if (method == "connect")     return method_connect(params);
    if (method == "disconnect")  return method_disconnect(params);
    if (method == "get_uia_tree")    return method_get_tree(params, true);
    if (method == "get_visual_tree") return method_get_tree(params, false);
    if (method == "get_frameworks")  return method_get_frameworks(params);
    if (method == "find_elements")   return method_find_elements(params);
    if (method == "get_element_properties") return method_get_element_properties(params);
    if (method == "screenshot")  return method_screenshot(params);
    if (method == "hit_test")    return method_hit_test(params);

#ifdef LVT_ENABLE_UIA
    if (method == "click")        return method_action(params, lvt::ActionKind::click, "click");
    if (method == "invoke")       return method_action(params, lvt::ActionKind::invoke, "invoke");
    if (method == "toggle")       return method_action(params, lvt::ActionKind::toggle, "toggle");
    if (method == "set_value")    return method_action(params, lvt::ActionKind::setValue, "set-value");
    if (method == "expand")       return method_action(params, lvt::ActionKind::expand, "expand");
    if (method == "collapse")     return method_action(params, lvt::ActionKind::collapse, "collapse");
    if (method == "select")       return method_action(params, lvt::ActionKind::select, "select");
    if (method == "add_to_selection")
        return method_action(params, lvt::ActionKind::addToSelection, "add-to-selection");
    if (method == "remove_from_selection")
        return method_action(params, lvt::ActionKind::removeFromSelection, "remove-from-selection");
    if (method == "select_text")  return method_action(params, lvt::ActionKind::selectText, "select-text");
    if (method == "focus")        return method_action(params, lvt::ActionKind::focus, "focus");
    if (method == "scroll")       return method_action(params, lvt::ActionKind::scroll, "scroll");
    if (method == "type_text")    return method_action(params, lvt::ActionKind::typeText, "type");
    if (method == "press_key")    return method_action(params, lvt::ActionKind::pressKey, "press-key");
    if (method == "close_window") return method_action(params, lvt::ActionKind::windowClose, "close");
    if (method == "minimize_window")
        return method_action(params, lvt::ActionKind::windowMinimize, "minimize");
    if (method == "maximize_window")
        return method_action(params, lvt::ActionKind::windowMaximize, "maximize");
    if (method == "restore_window")
        return method_action(params, lvt::ActionKind::windowRestore, "restore");
    if (method == "wait_for")     return method_action(params, lvt::ActionKind::waitFor, "wait-for");
    if (method == "wait_gone")    return method_action(params, lvt::ActionKind::waitGone, "wait-gone");
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
                                char** result_json) {
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
        payload = dispatch(method, params).dump();
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
