#include "target.h"
#include "framework_detector.h"
#include "tree_builder.h"
#include "json_serializer.h"
#include "watch_diff.h"
#include "screenshot.h"
#include "plugin_loader.h"
#include "debug.h"
#include "wil_diagnostics.h"
#include "lvt_config.h"
#ifdef LVT_ENABLE_UIA
#include "providers/uia_provider.h"
#include "providers/uia_actions.h"
#endif

#include "element_key.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

static std::atomic_bool g_watchStop = false;

static BOOL WINAPI console_ctrl_handler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT ||
        ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_LOGOFF_EVENT ||
        ctrlType == CTRL_SHUTDOWN_EVENT) {
        g_watchStop = true;
        return TRUE;
    }
    return FALSE;
}

static void print_usage() {
    fprintf(stderr,
        "lvt - Live Visual Tree inspector\n"
        "\n"
        "Usage:\n"
        "  lvt [verb] [arguments] [options]\n"
        "\n"
        "  A target is required: --hwnd, --pid, --name, or --title.\n"
        "  The verb defaults to 'dump' when omitted.\n"
        "\n"
        "Inspection verbs:\n"
        "  dump                     Output the element tree (default)\n"
        "  screenshot               Capture an annotated PNG (see --output)\n"
        "  frameworks               List the UI frameworks the target uses\n"
        "  watch                    Emit live JSON tree diff events until Ctrl+C\n"
        "  query <ref> [property]   Output one element, or one of its properties\n"
        "\n"
        "Interaction verbs (drive the app; each implies --uia):\n"
        "  click <ref>              Invoke, else default action, else a real click\n"
        "  right-click <ref>        Always a synthetic click\n"
        "  double-click <ref>       Always a synthetic double click\n"
        "  invoke <ref>             InvokePattern only, never a synthetic click\n"
        "  toggle <ref>             Flip a checkbox or toggle button\n"
        "  set-value <ref> <text>   Set a text, or numeric slider/spinner, value\n"
        "  expand <ref> | collapse <ref>\n"
        "  select <ref>             Select an item, replacing the selection\n"
        "  add-to-selection <ref> | remove-from-selection <ref>\n"
        "  select-text <ref> [text] Select a substring, or all text when omitted\n"
        "  focus <ref>              Move keyboard focus to an element\n"
        "  scroll <ref> <dir>       Scroll up, down, left, or right\n"
        "  type <text>              Type text; add --focus-first <ref> to target it\n"
        "  press-key <chord>        Send \"Ctrl+S\", or \"Enter;Tab\" for a sequence\n"
        "  close | minimize | maximize | restore [<ref>]\n"
        "                           Window commands; default to the target window\n"
        "  wait-for <ref>           Block until an element appears\n"
        "  wait-gone <ref>          Block until an element disappears\n"
        "\n"
        "Server verb:\n"
        "  mcp                      Serve the Model Context Protocol over stdio, so an\n"
        "                           agent can inspect and drive apps through lvt.\n"
        "                           Add --allow-input to expose the tools that click,\n"
        "                           type and otherwise change the target app.\n"
        "\n"
        "A <ref> is an element id (e5), a durable key, or uia:<RuntimeId>.\n"
        "\n"
        "Target options:\n"
        "  --hwnd <handle>      Target window by HWND (hex, e.g. 0x1A0B3C)\n"
        "  --pid <pid>          Target process by PID (finds main window)\n"
        "  --name <exe>         Target by process name (e.g. notepad.exe)\n"
        "  --title <text>       Target by window title substring; with Chromium and\n"
        "                       --name/--pid/--hwnd, select tab by URL/title substring\n"
        "\n"
        "Output options:\n"
        "  --output <file>      Write to a file instead of stdout (or the PNG path)\n"
        "  --format <fmt>       Output format: json (default) or xml\n"
        "  --element <ref>      Scope the tree to one element's subtree\n"
        "  --depth <n>          Max tree traversal depth (default: unlimited)\n"
        "  --interval <ms>      Watch polling interval (default: 500)\n"
#ifndef NDEBUG
        "  --annotations-json <file>  Write annotation rectangles as JSON (test hook)\n"
#endif
        "\n"
        "UI Automation options:\n"
        "  --uia                Use the UI Automation tree instead of the visual tree\n"
        "  --uia-view <view>    UIA tree view: control (default), raw, or content\n"
        "  --uia-props <list>   Comma-separated extra UIA properties to include\n"
        "  --uia-timeout <ms>   UIA walk deadline (default: 10000; 0 leaves UIA's own)\n"
        "\n"
        "Interaction options:\n"
        "  --focus-first <ref>  Focus this element before type / press-key\n"
        "  --wait-prop <name=value>  Narrow wait-for to a property value\n"
        "  --wait-timeout <ms>  How long wait-for / wait-gone block (default: 5000)\n"
        "\n"
        "  --debug              Show verbose diagnostic output\n"
        "  --help               Show this help\n"
        "  --version            Show the lvt version\n"
    );
}

// The verb selects what lvt does. Inspection verbs read; interaction verbs
// drive the app. Keeping them positional makes them mutually exclusive by
// construction, rather than by a runtime check across a dozen flags.
enum class Verb {
    dump, screenshot, frameworks, watch, query,
    click, rightClick, doubleClick, invoke, toggle, setValue,
    expand, collapse, select, addToSelection, removeFromSelection, selectText,
    focus, scroll, type, pressKey,
    close, minimize, maximize, restore,
    waitFor, waitGone,
    mcp,
};

struct Args {
    Verb verb = Verb::dump;
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::string processName;
    std::string windowTitle;
    std::string outputFile;
    std::string format = "json";
#ifndef NDEBUG
    std::string annotationsFile;
#endif
    std::string elementId;
    std::string pluginOption;
    // Positional arguments belonging to the verb, in order.
    std::vector<std::string> verbArgs;
    bool uia = false;
    std::string uiaViewName = "control";
    std::vector<std::string> uiaProps;
    int uiaTimeoutMs = 10000;
    std::string focusFirstRef;
    std::string waitProperty;
    std::string waitValue;
    int waitTimeoutMs = 5000;
    int depth = -1;
    int intervalMs = 500;
    // MCP only: expose the tools that can change the target application.
    bool allowInput = false;
};

static std::vector<std::string> split_csv(const std::string& text) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        auto piece = text.substr(start, comma == std::string::npos ? std::string::npos
                                                                   : comma - start);
        // Tolerate "a, b , c" so the flag is forgiving about spacing.
        const size_t first = piece.find_first_not_of(" \t");
        const size_t last = piece.find_last_not_of(" \t");
        if (first != std::string::npos)
            parts.push_back(piece.substr(first, last - first + 1));
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return parts;
}

// Reject a value that is not a whole non-negative number, rather than letting
// atoi turn a typo into 0.
static int parse_non_negative_int(const char* text, const char* flag) {
    char* end = nullptr;
    const long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > INT_MAX) {
        fprintf(stderr, "lvt: %s must be a whole non-negative number\n", flag);
        exit(1);
    }
    return static_cast<int>(value);
}

struct VerbSpec {
    const char* name;
    Verb verb;
    int minArgs;
    int maxArgs;
    const char* usage;
};

// Verbs with differing min/max arg counts take an optional trailing argument.
static const VerbSpec kVerbs[] = {
    {"dump",                 Verb::dump,                0, 0, "dump"},
    {"screenshot",           Verb::screenshot,          0, 0, "screenshot [--output <file>]"},
    {"frameworks",           Verb::frameworks,          0, 0, "frameworks"},
    {"watch",                Verb::watch,               0, 0, "watch"},
    {"query",                Verb::query,               1, 2, "query <ref> [property]"},
    {"click",                Verb::click,               1, 1, "click <ref>"},
    {"right-click",          Verb::rightClick,          1, 1, "right-click <ref>"},
    {"double-click",         Verb::doubleClick,         1, 1, "double-click <ref>"},
    {"invoke",               Verb::invoke,              1, 1, "invoke <ref>"},
    {"toggle",               Verb::toggle,              1, 1, "toggle <ref>"},
    {"set-value",            Verb::setValue,            2, 2, "set-value <ref> <text>"},
    {"expand",               Verb::expand,              1, 1, "expand <ref>"},
    {"collapse",             Verb::collapse,            1, 1, "collapse <ref>"},
    {"select",               Verb::select,              1, 1, "select <ref>"},
    {"add-to-selection",     Verb::addToSelection,      1, 1, "add-to-selection <ref>"},
    {"remove-from-selection", Verb::removeFromSelection, 1, 1, "remove-from-selection <ref>"},
    {"select-text",          Verb::selectText,          1, 2, "select-text <ref> [text]"},
    {"focus",                Verb::focus,               1, 1, "focus <ref>"},
    {"scroll",               Verb::scroll,              2, 2, "scroll <ref> <up|down|left|right>"},
    {"type",                 Verb::type,                1, 1, "type <text>"},
    {"press-key",            Verb::pressKey,            1, 1, "press-key <chord>"},
    {"close",                Verb::close,               0, 1, "close [<ref>]"},
    {"minimize",             Verb::minimize,            0, 1, "minimize [<ref>]"},
    {"maximize",             Verb::maximize,            0, 1, "maximize [<ref>]"},
    {"restore",              Verb::restore,             0, 1, "restore [<ref>]"},
    {"wait-for",             Verb::waitFor,             1, 1, "wait-for <ref>"},
    {"wait-gone",            Verb::waitGone,            1, 1, "wait-gone <ref>"},
    {"mcp",                  Verb::mcp,                 0, 0, "mcp [--allow-input]"},
};

static const VerbSpec* find_verb(const std::string& name) {
    for (const auto& spec : kVerbs) {
        if (name == spec.name)
            return &spec;
    }
    return nullptr;
}

// The MCP server lives in a Rust staticlib linked into this binary, so `lvt mcp`
// is served by lvt.exe itself rather than a second executable. The declaration
// is here rather than in a header because it is the only cross-language symbol
// and giving it a header would imply a wider seam than exists.
#ifdef LVT_ENABLE_MCP
extern "C" int lvt_mcp_serve_stdio(bool allow_input);
#endif

static int run_mcp_server(bool allowInput) {
#ifdef LVT_ENABLE_MCP
    // stdout carries the JSON-RPC stream from here on, so nothing else may
    // write to it. lvt's diagnostics already go to stderr; this makes the
    // requirement explicit at the one place it becomes load-bearing.
    const int result = lvt_mcp_serve_stdio(allowInput);

    // Leave immediately rather than returning through normal shutdown.
    //
    // A tool call runs on a thread that cannot be cancelled, so one still in
    // flight when the client disconnects — a `wait_for` part-way through its
    // deadline, say — would otherwise keep the process alive until it finished.
    // A host that closes the pipe expects the server to be gone, and work that
    // has no client left to answer is not worth waiting for. Everything the
    // caller can observe is already flushed by this point; what is skipped is
    // process-teardown bookkeeping the OS is about to do anyway.
    fflush(nullptr);
    _exit(result);
#else
    (void)allowInput;
    fprintf(stderr,
            "lvt: this build has no MCP server. Rebuild with -DLVT_ENABLE_MCP=ON, "
            "which needs a Rust toolchain (https://rustup.rs).\n");
    return 1;
#endif
}

static bool verb_drives_app(Verb verb) {
    switch (verb) {
    case Verb::dump: case Verb::screenshot: case Verb::frameworks:
    case Verb::watch: case Verb::query:
    // mcp is a server, not an action: it is dispatched before target
    // resolution and never reaches run_action.
    case Verb::mcp:
        return false;
    default:
        return true;
    }
}

// These were flags before verbs existed. Failing with the replacement is far
// more useful than "unknown argument", since the whole shape of the command
// changed rather than just a spelling.
static void reject_legacy_flag(const char* arg) {
    struct Legacy { const char* flag; const char* replacement; };
    static const Legacy kLegacy[] = {
        {"--dump",       "the 'dump' verb (also the default): lvt dump --name <exe>"},
        {"--screenshot", "the 'screenshot' verb: lvt screenshot --name <exe> --output <file>"},
        {"--frameworks", "the 'frameworks' verb: lvt frameworks --name <exe>"},
        {"--watch",      "the 'watch' verb: lvt watch --name <exe>"},
        {"--query",      "the 'query' verb: lvt query <ref> [property] --name <exe>"},
    };
    for (const auto& legacy : kLegacy) {
        if (strcmp(arg, legacy.flag) == 0) {
            fprintf(stderr, "lvt: %s is now %s\n", legacy.flag, legacy.replacement);
            fprintf(stderr, "lvt: run 'lvt --help' for the full verb list\n");
            exit(1);
        }
    }
}

static Args parse_args(int argc, char* argv[]) {
    Args args;
    const VerbSpec* spec = nullptr;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage();
            exit(0);
        }

        if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
            printf("lvt %s\n", LVT_VERSION);
            exit(0);
        }

        if (strncmp(arg, "--", 2) != 0) {
            // A bare word is either the verb, when it comes first, or an
            // argument to the verb already chosen.
            if (!spec) {
                spec = find_verb(arg);
                if (!spec) {
                    fprintf(stderr, "lvt: unknown verb '%s'\n", arg);
                    fprintf(stderr, "lvt: run 'lvt --help' for the verb list\n");
                    exit(1);
                }
                args.verb = spec->verb;
                if (verb_drives_app(spec->verb))
                    args.uia = true;
                continue;
            }
            args.verbArgs.push_back(arg);
            continue;
        }

        reject_legacy_flag(arg);

        if (strcmp(arg, "--hwnd") == 0 && i + 1 < argc) {
            auto val = strtoull(argv[++i], nullptr, 0);
            args.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(val));
        } else if (strcmp(arg, "--pid") == 0 && i + 1 < argc) {
            args.pid = static_cast<DWORD>(strtoul(argv[++i], nullptr, 10));
        } else if (strcmp(arg, "--name") == 0 && i + 1 < argc) {
            args.processName = argv[++i];
        } else if (strcmp(arg, "--title") == 0 && i + 1 < argc) {
            args.windowTitle = argv[++i];
        } else if (strcmp(arg, "--output") == 0 && i + 1 < argc) {
            args.outputFile = argv[++i];
        } else if (strcmp(arg, "--format") == 0 && i + 1 < argc) {
            args.format = argv[++i];
#ifndef NDEBUG
        } else if (strcmp(arg, "--annotations-json") == 0 && i + 1 < argc) {
            args.annotationsFile = argv[++i];
#endif
        } else if (strcmp(arg, "--element") == 0 && i + 1 < argc) {
            args.elementId = argv[++i];
        } else if (strcmp(arg, "--depth") == 0 && i + 1 < argc) {
            args.depth = parse_non_negative_int(argv[++i], "--depth");
        } else if (strcmp(arg, "--interval") == 0 && i + 1 < argc) {
            args.intervalMs = parse_non_negative_int(argv[++i], "--interval");
        } else if (strcmp(arg, "--uia") == 0) {
            args.uia = true;
        } else if (strcmp(arg, "--allow-input") == 0) {
            args.allowInput = true;
        } else if (strcmp(arg, "--uia-view") == 0 && i + 1 < argc) {
            args.uia = true;
            args.uiaViewName = argv[++i];
        } else if (strcmp(arg, "--uia-props") == 0 && i + 1 < argc) {
            args.uia = true;
            args.uiaProps = split_csv(argv[++i]);
        } else if (strcmp(arg, "--uia-timeout") == 0 && i + 1 < argc) {
            args.uia = true;
            args.uiaTimeoutMs = parse_non_negative_int(argv[++i], "--uia-timeout");
        } else if (strcmp(arg, "--focus-first") == 0 && i + 1 < argc) {
            args.focusFirstRef = argv[++i];
            args.uia = true;
        } else if (strcmp(arg, "--wait-prop") == 0 && i + 1 < argc) {
            const std::string pair = argv[++i];
            const size_t eq = pair.find('=');
            if (eq == std::string::npos || eq == 0) {
                fprintf(stderr, "lvt: --wait-prop must be <name>=<value>\n");
                exit(1);
            }
            args.waitProperty = pair.substr(0, eq);
            args.waitValue = pair.substr(eq + 1);
        } else if (strcmp(arg, "--wait-timeout") == 0 && i + 1 < argc) {
            args.waitTimeoutMs = parse_non_negative_int(argv[++i], "--wait-timeout");
        } else if (strcmp(arg, "--debug") == 0) {
            lvt::g_debug = true;
        } else {
            fprintf(stderr, "lvt: unknown argument '%s'\n", arg);
            print_usage();
            exit(1);
        }
    }

    if (!spec)
        spec = find_verb("dump");

    // A uia:<RuntimeId> reference only exists in a UIA tree, so asking for one
    // implies that view. Without this the lookup would search the visual tree
    // and report a confusing "not found".
    auto impliesUia = [](const std::string& ref) {
        return ref.rfind("uia:", 0) == 0;
    };
    if (impliesUia(args.elementId))
        args.uia = true;
    for (const auto& verbArg : args.verbArgs) {
        if (impliesUia(verbArg))
            args.uia = true;
    }

    const int count = static_cast<int>(args.verbArgs.size());
    if (count < spec->minArgs || count > spec->maxArgs) {
        fprintf(stderr, "lvt: usage: lvt %s\n", spec->usage);
        exit(1);
    }
    return args;
}


static std::vector<std::string> framework_names(const std::vector<lvt::FrameworkInfo>& frameworks) {
    std::vector<std::string> names;
    for (auto& fi : frameworks) {
        auto name = lvt::framework_display_name(fi);
        if (fi.version.empty())
            names.push_back(name);
        else
            names.push_back(name + " " + fi.version);
    }
    return names;
}

static std::string xml_escape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':  r += "&amp;";  break;
        case '<':  r += "&lt;";   break;
        case '>':  r += "&gt;";   break;
        case '"':  r += "&quot;"; break;
        case '\'': r += "&apos;"; break;
        default:
            if (static_cast<unsigned char>(c) >= 0x20)
                r += c;
        }
    }
    return r;
}

static nlohmann::json query_element_to_json(const lvt::Element& element) {
    nlohmann::json j;
    j["id"] = element.id;
    j["key"] = element.key;
    j["type"] = element.type;
    j["framework"] = element.framework;
    j["className"] = element.className;
    j["text"] = element.text;
    j["bounds"] = lvt::get_element_property(element, "bounds").value();
    for (auto& [key, value] : element.properties)
        j[key] = value;
    return j;
}

static std::string query_element_to_xml(const lvt::Element& element) {
    std::ostringstream out;
    out << "<Element";
    out << " id=\"" << xml_escape(element.id) << "\"";
    out << " key=\"" << xml_escape(element.key) << "\"";
    out << " type=\"" << xml_escape(element.type) << "\"";
    out << " framework=\"" << xml_escape(element.framework) << "\"";
    out << " className=\"" << xml_escape(element.className) << "\"";
    out << " text=\"" << xml_escape(element.text) << "\"";
    out << " bounds=\"" << xml_escape(lvt::get_element_property(element, "bounds").value()) << "\"";
    for (auto& [key, value] : element.properties)
        out << " " << xml_escape(key) << "=\"" << xml_escape(value) << "\"";
    out << " />";
    return out.str();
}

static bool write_output(const std::string& outputFile, const std::string& content) {
    if (outputFile.empty()) {
        printf("%s\n", content.c_str());
        return true;
    }
    std::ofstream out(outputFile);
    if (!out) {
        fprintf(stderr, "lvt: cannot write to '%s'\n", outputFile.c_str());
        return false;
    }
    out << content << "\n";
    if (lvt::g_debug)
        fprintf(stderr, "lvt: wrote output to %s\n", outputFile.c_str());
    return true;
}

// UIA support is compile-time optional (LVT_ENABLE_UIA). Keep the two entry
// points behind a single seam so the rest of main.cpp does not need guards.
#ifdef LVT_ENABLE_UIA
static bool build_uia_tree(const lvt::TargetInfo& target, const Args& args,
                           lvt::Element& tree) {
    lvt::UiaOptions options;
    if (!lvt::parse_uia_view(args.uiaViewName, options.view)) {
        fprintf(stderr, "lvt: --uia-view must be raw, control, or content\n");
        return false;
    }
    options.extraProperties = args.uiaProps;
    options.timeoutMs = args.uiaTimeoutMs;

    lvt::UiaProvider provider;
    auto result = provider.build(target.hwnd, options);
    if (!result) {
        fprintf(stderr, "lvt: could not read the UI Automation tree for this window");
        if (options.timeoutMs > 0) {
            fprintf(stderr, " (a slow or busy target may need a larger "
                            "--uia-timeout than %d ms)", options.timeoutMs);
        }
        fprintf(stderr, "\n");
        return false;
    }
    tree = std::move(*result);
    lvt::assign_element_ids(tree);
    lvt::assign_element_keys(tree);
    return true;
}

static std::string uia_framework_label(const Args& args) {
    lvt::UiaView view = lvt::UiaView::control;
    lvt::parse_uia_view(args.uiaViewName, view);
    return std::string("uia (") + lvt::uia_view_name(view) + " view)";
}
#else
static bool build_uia_tree(const lvt::TargetInfo&, const Args&, lvt::Element&) {
    fprintf(stderr, "lvt: this build has UI Automation support compiled out "
                    "(LVT_ENABLE_UIA=OFF)\n");
    return false;
}

static std::string uia_framework_label(const Args&) { return "uia"; }
#endif

// Build the root tree for the requested mode. --uia replaces the visual tree
// outright rather than enriching it: it is a different view of the same window,
// produced without injecting anything into the target.
static bool build_root_tree(const lvt::TargetInfo& target, const Args& args,
                            lvt::Element& tree) {
    if (args.uia) {
        if (!build_uia_tree(target, args, tree))
            return false;
        return true;
    }

    auto frameworks = lvt::detect_frameworks(target.hwnd, target.pid);
    tree = lvt::build_tree(target.hwnd, target.pid, frameworks, -1, args.pluginOption);
    return true;
}

static bool build_output_tree(const lvt::TargetInfo& target, const Args& args,
                              lvt::Element& outputTree) {
    lvt::Element tree;
    if (!build_root_tree(target, args, tree))
        return false;

    lvt::Element* outputRoot = &tree;
    if (!args.elementId.empty()) {
        outputRoot = lvt::find_element_by_ref(tree, args.elementId);
        if (!outputRoot) {
            fprintf(stderr, "lvt: element '%s' not found\n", args.elementId.c_str());
            return false;
        }
    }

    if (args.depth >= 0)
        lvt::trim_to_depth(*outputRoot, args.depth);

    outputTree = *outputRoot;
    return true;
}

static int run_watch_loop(const lvt::TargetInfo& target, const Args& args) {
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    lvt::Element previous;
    if (!build_output_tree(target, args, previous))
        return 1;

    for (const auto& event : lvt::snapshot_added_events(previous))
        printf("%s\n", lvt::serialize_change_event(event).c_str());
    fflush(stdout);

    while (!g_watchStop) {
        auto sleepUntil = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(args.intervalMs);
        while (!g_watchStop && std::chrono::steady_clock::now() < sleepUntil) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (g_watchStop)
            break;

        if (!IsWindow(target.hwnd)) {
            fprintf(stderr, "lvt: target window closed\n");
            break;
        }

        lvt::Element current;
        if (!build_output_tree(target, args, current)) {
            // A tick can fail transiently — most easily in UIA mode, where a
            // momentarily busy target trips the transaction timeout. That is
            // precisely the condition --watch exists to observe, so skip the
            // tick and keep watching rather than ending the session.
            if (lvt::g_debug)
                fprintf(stderr, "lvt: skipping watch tick; tree unavailable\n");
            continue;
        }

        for (const auto& event : lvt::diff_trees(previous, current))
            printf("%s\n", lvt::serialize_change_event(event).c_str());
        fflush(stdout);
        previous = std::move(current);
    }

    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    return 0;
}

// Translate a verb and its positional arguments into an action request. The
// mapping lives here so uia_actions stays free of CLI shape.
#ifdef LVT_ENABLE_UIA
static bool build_action_request(const Args& args, lvt::ActionRequest& request) {
    auto arg = [&](size_t index) -> std::string {
        return index < args.verbArgs.size() ? args.verbArgs[index] : std::string();
    };

    switch (args.verb) {
    case Verb::click:
        request.kind = lvt::ActionKind::click;
        request.elementRef = arg(0);
        break;
    case Verb::rightClick:
        // No pattern expresses "right click", so this is always synthetic.
        request.kind = lvt::ActionKind::click;
        request.elementRef = arg(0);
        request.button = 1;
        request.forceSyntheticClick = true;
        break;
    case Verb::doubleClick:
        request.kind = lvt::ActionKind::click;
        request.elementRef = arg(0);
        request.amount = 2;
        request.forceSyntheticClick = true;
        break;
    case Verb::invoke:
        request.kind = lvt::ActionKind::invoke;
        request.elementRef = arg(0);
        break;
    case Verb::toggle:
        request.kind = lvt::ActionKind::toggle;
        request.elementRef = arg(0);
        break;
    case Verb::setValue:
        request.kind = lvt::ActionKind::setValue;
        request.elementRef = arg(0);
        request.text = arg(1);
        break;
    case Verb::expand:
        request.kind = lvt::ActionKind::expand;
        request.elementRef = arg(0);
        break;
    case Verb::collapse:
        request.kind = lvt::ActionKind::collapse;
        request.elementRef = arg(0);
        break;
    case Verb::select:
        request.kind = lvt::ActionKind::select;
        request.elementRef = arg(0);
        break;
    case Verb::addToSelection:
        request.kind = lvt::ActionKind::addToSelection;
        request.elementRef = arg(0);
        break;
    case Verb::removeFromSelection:
        request.kind = lvt::ActionKind::removeFromSelection;
        request.elementRef = arg(0);
        break;
    case Verb::selectText:
        request.kind = lvt::ActionKind::selectText;
        request.elementRef = arg(0);
        request.text = arg(1);
        break;
    case Verb::focus:
        request.kind = lvt::ActionKind::focus;
        request.elementRef = arg(0);
        break;
    case Verb::scroll:
        request.kind = lvt::ActionKind::scroll;
        request.elementRef = arg(0);
        request.direction = arg(1);
        if (request.direction != "up" && request.direction != "down" &&
            request.direction != "left" && request.direction != "right") {
            fprintf(stderr, "lvt: scroll direction must be up, down, left, or right\n");
            return false;
        }
        break;
    case Verb::type:
        request.kind = lvt::ActionKind::typeText;
        request.text = arg(0);
        request.elementRef = args.focusFirstRef;
        break;
    case Verb::pressKey:
        request.kind = lvt::ActionKind::pressKey;
        request.text = arg(0);
        request.elementRef = args.focusFirstRef;
        break;
    case Verb::close:
    case Verb::minimize:
    case Verb::maximize:
    case Verb::restore:
        request.kind = args.verb == Verb::close    ? lvt::ActionKind::windowClose
                     : args.verb == Verb::minimize ? lvt::ActionKind::windowMinimize
                     : args.verb == Verb::maximize ? lvt::ActionKind::windowMaximize
                                                   : lvt::ActionKind::windowRestore;
        // Window commands default to the target window itself, which is the
        // root of the walk.
        request.elementRef = args.verbArgs.empty() ? "e0" : arg(0);
        break;
    case Verb::waitFor:
    case Verb::waitGone:
        request.kind = args.verb == Verb::waitFor ? lvt::ActionKind::waitFor
                                                  : lvt::ActionKind::waitGone;
        request.elementRef = arg(0);
        request.waitProperty = args.waitProperty;
        request.waitValue = args.waitValue;
        request.waitTimeoutMs = args.waitTimeoutMs;
        break;
    default:
        return false;
    }
    return true;
}
#endif

// Carry out an interaction and report the outcome as JSON on stdout, so a
// caller can tell not just whether it worked but *how* — a UIA pattern and a
// synthetic click have very different reliability.
static int run_action(const lvt::TargetInfo& target, const Args& args) {
#ifdef LVT_ENABLE_UIA
    lvt::UiaOptions options;
    if (!lvt::parse_uia_view(args.uiaViewName, options.view)) {
        fprintf(stderr, "lvt: --uia-view must be raw, control, or content\n");
        return 1;
    }
    options.extraProperties = args.uiaProps;
    options.timeoutMs = args.uiaTimeoutMs;

    lvt::ActionRequest request;
    if (!build_action_request(args, request))
        return 1;

    const auto result = lvt::perform_action(target.hwnd, options, request);

    nlohmann::json out;
    out["action"] = lvt::action_kind_name(request.kind);
    out["ok"] = result.ok;
    if (!request.elementRef.empty())
        out["element"] = request.elementRef;
    if (!result.method.empty())
        out["method"] = result.method;
    if (!result.message.empty())
        out["error"] = result.message;
    if (result.hasElement)
        out["result"] = query_element_to_json(result.element);

    if (!write_output(args.outputFile, out.dump(2)))
        return 1;
    return result.ok ? 0 : 1;
#else
    (void)target; (void)args;
    fprintf(stderr, "lvt: this build has UI Automation support compiled out "
                    "(LVT_ENABLE_UIA=OFF), so interaction is unavailable\n");
    return 1;
#endif
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    auto args = parse_args(argc, argv);

    // Route WIL failures to stderr now that --debug is known. stdout carries the
    // tree payload, so diagnostics must never go there.
    lvt::install_wil_result_logger();

    bool hasNonTitleTarget = args.hwnd || args.pid || !args.processName.empty();
    args.pluginOption = hasNonTitleTarget ? args.windowTitle : "";

    // Load plugins from %USERPROFILE%/.lvt/plugins/
    lvt::load_plugins();

    if (args.intervalMs <= 0) {
        fprintf(stderr, "lvt: --interval must be greater than 0\n");
        return 1;
    }
    if (args.verb == Verb::watch && args.format != "json") {
        fprintf(stderr, "lvt: watch emits JSON events; --format must be json\n");
        return 1;
    }
    if (args.format != "json" && args.format != "xml") {
        fprintf(stderr, "lvt: --format must be json or xml\n");
        return 1;
    }

    // The MCP server chooses its own targets through its connect tool, so it
    // runs before any of the target resolution below and never needs --hwnd.
    if (args.verb == Verb::mcp)
        return run_mcp_server(args.allowInput);

    if (!args.hwnd && !args.pid && args.processName.empty() && args.windowTitle.empty()) {
        fprintf(stderr, "lvt: must specify --hwnd, --pid, --name, or --title\n");
        return 1;
    }

    // Resolve target via --name or --title (with multi-match handling)
    if (!args.processName.empty()) {
        auto matches = lvt::find_by_process_name(args.processName);
        if (matches.empty()) {
            fprintf(stderr, "lvt: no visible windows found for process '%s'\n",
                    args.processName.c_str());
            return 1;
        }
        if (matches.size() > 1) {
            fprintf(stderr, "lvt: multiple windows match '%s':\n", args.processName.c_str());
            for (auto& m : matches) {
                fprintf(stderr, "  --hwnd 0x%p  pid=%lu  %s  \"%s\"\n",
                        static_cast<void*>(m.hwnd), m.pid,
                        m.processName.c_str(), m.windowTitle.c_str());
            }
            return 1;
        }
        args.hwnd = matches[0].hwnd;
    } else if (!args.windowTitle.empty()) {
        auto matches = lvt::find_by_title(args.windowTitle);
        if (matches.empty()) {
            fprintf(stderr, "lvt: no visible windows found with title containing '%s'\n",
                    args.windowTitle.c_str());
            return 1;
        }
        if (matches.size() > 1) {
            fprintf(stderr, "lvt: multiple windows match title '%s':\n",
                    args.windowTitle.c_str());
            for (auto& m : matches) {
                fprintf(stderr, "  --hwnd 0x%p  pid=%lu  %s  \"%s\"\n",
                        static_cast<void*>(m.hwnd), m.pid,
                        m.processName.c_str(), m.windowTitle.c_str());
            }
            return 1;
        }
        args.hwnd = matches[0].hwnd;
    }

    // Resolve target
    auto target = lvt::resolve_target(args.hwnd, args.pid);
    if (!target.hwnd) {
        fprintf(stderr, "lvt: could not find window for target\n");
        return 1;
    }
    if (!IsWindow(target.hwnd)) {
        // wait-gone is the one verb whose success condition is the window
        // being gone, so an invalid handle satisfies it rather than failing it.
        if (args.verb == Verb::waitGone) {
            printf("%s\n", nlohmann::json{{"action", "wait-gone"},
                                          {"ok", true},
                                          {"element", args.verbArgs.empty()
                                                          ? std::string()
                                                          : args.verbArgs[0]},
                                          {"method", "window-closed"}}
                               .dump(2)
                               .c_str());
            return 0;
        }
        fprintf(stderr, "lvt: target HWND 0x%p is not a valid window\n",
                static_cast<void*>(target.hwnd));
        return 1;
    }

    // Check architecture match. --uia is exempt: UI Automation is a cross-process,
    // cross-architecture client API, so an x64 lvt can read an ARM64 or x86
    // target's UIA tree. Only the injecting visual-tree providers need a match.
    auto hostArch = lvt::get_host_architecture();
    if (!args.uia &&
        target.architecture != lvt::Architecture::unknown &&
        hostArch != lvt::Architecture::unknown &&
        target.architecture != hostArch) {
        const char* targetArchName = lvt::architecture_name(target.architecture);
        const char* hostArchName = lvt::architecture_name(hostArch);
        fprintf(stderr,
            "lvt: architecture mismatch - this is lvt.exe (%s) but the target process "
            "(pid %lu) is %s.\nRun lvt-%s.exe instead, or use --uia which works "
            "across architectures.\n",
            hostArchName, target.pid, targetArchName, targetArchName);
        return 1;
    }

    // Detect frameworks. Skipped for --uia, which reports the per-element
    // FrameworkId from UIA instead and needs no module enumeration.
    std::vector<lvt::FrameworkInfo> frameworks;
    if (!args.uia || args.verb == Verb::frameworks)
        frameworks = lvt::detect_frameworks(target.hwnd, target.pid);

    if (args.verb == Verb::frameworks) {
        // Just print detected frameworks
        for (auto& fi : frameworks) {
            auto name = lvt::framework_display_name(fi);
            if (fi.version.empty())
                printf("%s\n", name.c_str());
            else
                printf("%s %s\n", name.c_str(), fi.version.c_str());
        }
        lvt::unload_plugins();
        return 0;
    }

    if (verb_drives_app(args.verb)) {
        auto result = run_action(target, args);
        lvt::unload_plugins();
        return result;
    }

    if (args.verb == Verb::watch) {
        auto result = run_watch_loop(target, args);
        lvt::unload_plugins();
        return result;
    }

    // Build full tree (no depth limit) so element IDs are stable
    lvt::Element tree;
    if (!build_root_tree(target, args, tree)) {
        lvt::unload_plugins();
        return 1;
    }

    // Scope to element if requested
    lvt::Element* outputRoot = &tree;
    if (!args.elementId.empty()) {
        outputRoot = lvt::find_element_by_ref(tree, args.elementId);
        if (!outputRoot) {
            fprintf(stderr, "lvt: element '%s' not found\n", args.elementId.c_str());
            return 1;
        }
    }

    if (args.verb == Verb::query) {
        const std::string& queryRef = args.verbArgs[0];
        const std::string queryProperty =
            args.verbArgs.size() > 1 ? args.verbArgs[1] : std::string();

        auto* queryElement = lvt::find_element_by_ref(tree, queryRef);
        if (!queryElement) {
            fprintf(stderr, "lvt: element '%s' not found\n", queryRef.c_str());
            return 1;
        }

        std::string queryOutput;
        if (!queryProperty.empty()) {
            auto value = lvt::get_element_property(*queryElement, queryProperty);
            if (!value) {
                fprintf(stderr, "lvt: property '%s' not found on element '%s'\n",
                        queryProperty.c_str(), queryRef.c_str());
                return 1;
            }
            queryOutput = *value;
        } else if (args.format == "xml") {
            queryOutput = query_element_to_xml(*queryElement);
        } else {
            queryOutput = query_element_to_json(*queryElement).dump(2);
        }

        if (!write_output(args.outputFile, queryOutput))
            return 1;
        lvt::unload_plugins();
        return 0;
    }

    // Apply depth limit relative to the output root
    if (args.depth >= 0) {
        lvt::trim_to_depth(*outputRoot, args.depth);
    }

    if (args.verb == Verb::dump) {
        auto frameworkNames = framework_names(frameworks);
        // In UIA mode the module-scan framework list is not what was walked;
        // report the view actually produced instead.
        if (args.uia)
            frameworkNames = {uia_framework_label(args)};
        std::string serialized;
        if (args.format == "xml") {
            serialized = lvt::serialize_to_xml(*outputRoot, target.hwnd, target.pid,
                                                target.processName, frameworkNames);
        } else {
            serialized = lvt::serialize_to_json(*outputRoot, target.hwnd, target.pid,
                                                 target.processName, frameworkNames);
        }

        if (!write_output(args.outputFile, serialized))
            return 1;
    }

    if (args.verb == Verb::screenshot) {
        // The verb decides the intent, so --output names the PNG here just as
        // it names the tree file for dump.
        const std::string path = args.outputFile.empty() ? "lvt-screenshot.png"
                                                         : args.outputFile;
        if (!lvt::capture_screenshot(target.hwnd, path, &tree, args.elementId)) {
            fprintf(stderr, "lvt: could not capture a screenshot of this window\n");
            lvt::unload_plugins();
            return 1;
        }
        if (lvt::g_debug)
            fprintf(stderr, "lvt: saved screenshot to %s\n", path.c_str());
    }

#ifndef NDEBUG
    // Annotations JSON — test hook for verifying which elements are annotated
    if (!args.annotationsFile.empty()) {
        auto annotations = lvt::collect_annotations(target.hwnd, &tree);
        nlohmann::json aj = nlohmann::json::array();
        for (auto& a : annotations) {
            aj.push_back({{"id", a.id}, {"x", a.x}, {"y", a.y},
                          {"width", a.width}, {"height", a.height}});
        }
        std::ofstream out(args.annotationsFile);
        if (out) {
            out << aj.dump(2) << "\n";
            if (lvt::g_debug)
                fprintf(stderr, "lvt: wrote %zu annotations to %s\n",
                        annotations.size(), args.annotationsFile.c_str());
        }
    }
#endif

    lvt::unload_plugins();
    return 0;
}
