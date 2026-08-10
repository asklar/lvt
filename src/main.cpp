#include "target.h"
#include "framework_detector.h"
#include "tree_builder.h"
#include "json_serializer.h"
#include "watch_diff.h"
#include "screenshot.h"
#include "plugin_loader.h"
#include "debug.h"
#include "wil_diagnostics.h"
#ifdef LVT_ENABLE_UIA
#include "providers/uia_provider.h"
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
        "  lvt --hwnd <handle>  [options]\n"
        "  lvt --pid <pid>      [options]\n"
        "  lvt --name <exe>     [options]\n"
        "  lvt --title <text>   [options]\n"
        "\n"
        "Options:\n"
        "  --hwnd <handle>      Target window by HWND (hex, e.g. 0x1A0B3C)\n"
        "  --pid <pid>          Target process by PID (finds main window)\n"
        "  --name <exe>         Target by process name (e.g. notepad.exe)\n"
        "  --title <text>       Target by window title substring; with Chromium and\n"
        "                       --name/--pid/--hwnd, select tab by URL/title substring\n"
        "  --output <file>      Write output to file instead of stdout\n"
        "  --format <fmt>       Output format: json (default) or xml\n"
        "  --screenshot <file>  Capture annotated screenshot to PNG\n"
#ifndef NDEBUG
        "  --annotations-json <file>  Write annotation rectangles as JSON (test hook)\n"
#endif
        "  --dump               Output the tree (default; implied unless --screenshot)\n"
        "  --watch              Emit live JSON tree diff events until Ctrl+C\n"
        "  --interval <ms>      Watch polling interval (default: 500)\n"
        "  --element <ref>      Scope to a specific element subtree by id or key\n"
        "  --query <ref> [prop] Output one element or one property by id or key\n"
        "  --uia                Emit the UI Automation tree instead of the visual tree\n"
        "  --uia-view <view>    UIA tree view: control (default), raw, or content\n"
        "  --uia-props <list>   Comma-separated extra UIA properties to include\n"
        "  --uia-timeout <ms>   UIA walk deadline (default: 10000; 0 leaves UIA's own)\n"
        "  --frameworks         Just detect and list frameworks\n"
        "  --depth <n>          Max tree traversal depth (default: unlimited)\n"
        "  --debug              Show verbose diagnostic output\n"
        "  --help               Show this help\n"
    );
}

struct Args {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::string processName;
    std::string windowTitle;
    std::string outputFile;
    std::string format = "json";
    std::string screenshotFile;
#ifndef NDEBUG
    std::string annotationsFile;
#endif
    std::string elementId;
    std::string queryId;
    std::string queryProperty;
    std::string pluginOption;
    bool uia = false;
    std::string uiaViewName = "control";
    std::vector<std::string> uiaProps;
    int uiaTimeoutMs = 10000;
    int depth = -1;
    int intervalMs = 500;
    bool frameworksOnly = false;
    bool dump = false;      // explicitly requested via --dump
    bool dumpSet = false;   // true if --dump was passed on command line
    bool watch = false;
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

static Args parse_args(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            exit(0);
        } else if (strcmp(argv[i], "--hwnd") == 0 && i + 1 < argc) {
            auto val = strtoull(argv[++i], nullptr, 0);
            args.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(val));
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            args.pid = static_cast<DWORD>(strtoul(argv[++i], nullptr, 10));
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            args.processName = argv[++i];
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            args.windowTitle = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            args.outputFile = argv[++i];
        } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            args.format = argv[++i];
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            args.screenshotFile = argv[++i];
#ifndef NDEBUG
        } else if (strcmp(argv[i], "--annotations-json") == 0 && i + 1 < argc) {
            args.annotationsFile = argv[++i];
#endif
        } else if (strcmp(argv[i], "--element") == 0 && i + 1 < argc) {
            args.elementId = argv[++i];
        } else if (strcmp(argv[i], "--query") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "lvt: --query requires an element id or key\n");
                exit(1);
            }
            args.queryId = argv[++i];
            if (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0)
                args.queryProperty = argv[++i];
        } else if (strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
            args.depth = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--uia") == 0) {
            args.uia = true;
        } else if (strcmp(argv[i], "--uia-view") == 0 && i + 1 < argc) {
            args.uia = true;
            args.uiaViewName = argv[++i];
        } else if (strcmp(argv[i], "--uia-props") == 0 && i + 1 < argc) {
            args.uia = true;
            args.uiaProps = split_csv(argv[++i]);
        } else if (strcmp(argv[i], "--uia-timeout") == 0 && i + 1 < argc) {
            args.uia = true;
            // atoi would turn a typo into 0, which silently *disables* the
            // deadline rather than rejecting the argument.
            char* end = nullptr;
            const long value = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value < 0) {
                fprintf(stderr, "lvt: --uia-timeout must be a whole number of "
                                "milliseconds (0 disables the deadline)\n");
                exit(1);
            }
            args.uiaTimeoutMs = static_cast<int>(value);
        } else if (strcmp(argv[i], "--watch") == 0) {
            args.watch = true;
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            args.intervalMs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--frameworks") == 0) {
            args.frameworksOnly = true;
        } else if (strcmp(argv[i], "--dump") == 0) {
            args.dump = true;
            args.dumpSet = true;
        } else if (strcmp(argv[i], "--debug") == 0) {
            lvt::g_debug = true;
        } else {
            fprintf(stderr, "lvt: unknown argument '%s'\n", argv[i]);
            print_usage();
            exit(1);
        }
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

    // --dump is default unless --screenshot is specified without --dump
    if (!args.dumpSet)
        args.dump = args.screenshotFile.empty();

    if (args.intervalMs <= 0) {
        fprintf(stderr, "lvt: --interval must be greater than 0\n");
        return 1;
    }
    if (args.watch && args.format != "json") {
        fprintf(stderr, "lvt: --watch emits JSON events; --format must be json\n");
        return 1;
    }
    if (args.format != "json" && args.format != "xml") {
        fprintf(stderr, "lvt: --format must be json or xml\n");
        return 1;
    }

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
    if (!args.uia || args.frameworksOnly)
        frameworks = lvt::detect_frameworks(target.hwnd, target.pid);

    if (args.frameworksOnly) {
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

    if (args.watch) {
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

    if (!args.queryId.empty()) {
        auto* queryElement = lvt::find_element_by_ref(tree, args.queryId);
        if (!queryElement) {
            fprintf(stderr, "lvt: element '%s' not found\n", args.queryId.c_str());
            return 1;
        }

        std::string queryOutput;
        if (!args.queryProperty.empty()) {
            auto value = lvt::get_element_property(*queryElement, args.queryProperty);
            if (!value) {
                fprintf(stderr, "lvt: property '%s' not found on element '%s'\n",
                        args.queryProperty.c_str(), args.queryId.c_str());
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

    // Serialize and output tree (unless suppressed by --screenshot without --dump)
    if (args.dump) {
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

    // Screenshot
    if (!args.screenshotFile.empty()) {
        bool ok = lvt::capture_screenshot(target.hwnd, args.screenshotFile,
                                          &tree, args.elementId);
        if (ok && lvt::g_debug) {
            fprintf(stderr, "lvt: saved screenshot to %s\n", args.screenshotFile.c_str());
        }
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
