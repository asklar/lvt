#include "target.h"
#include "framework_detector.h"
#include "tree_builder.h"
#include "json_serializer.h"
#include "screenshot.h"
#include "debug.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>

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
        "  --title <text>       Target by window title substring\n"
        "  --output <file>      Write output to file instead of stdout\n"
        "  --format <fmt>       Output format: json (default) or xml\n"
        "  --screenshot <file>  Capture annotated screenshot to PNG\n"
        "  --dump               Output the tree (default; implied unless --screenshot)\n"
        "  --element <id>       Scope to a specific element subtree\n"
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
    std::string elementId;
    int depth = -1;
    bool frameworksOnly = false;
    bool dump = false;      // explicitly requested via --dump
    bool dumpSet = false;   // true if --dump was passed on command line
};

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
        } else if (strcmp(argv[i], "--element") == 0 && i + 1 < argc) {
            args.elementId = argv[++i];
        } else if (strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
            args.depth = atoi(argv[++i]);
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

static lvt::Element* find_element(lvt::Element& root, const std::string& id) {
    if (root.id == id) return &root;
    for (auto& child : root.children) {
        auto* found = find_element(child, id);
        if (found) return found;
    }
    return nullptr;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    auto args = parse_args(argc, argv);

    // --dump is default unless --screenshot is specified without --dump
    if (!args.dumpSet)
        args.dump = args.screenshotFile.empty();

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

    // Detect frameworks
    auto frameworks = lvt::detect_frameworks(target.hwnd, target.pid);

    if (args.frameworksOnly) {
        // Just print detected frameworks
        for (auto& fi : frameworks) {
            if (fi.version.empty())
                printf("%s\n", lvt::framework_to_string(fi.type).c_str());
            else
                printf("%s %s\n", lvt::framework_to_string(fi.type).c_str(),
                       fi.version.c_str());
        }
        return 0;
    }

    // Build full tree (no depth limit) so element IDs are stable
    auto tree = lvt::build_tree(target.hwnd, target.pid, frameworks);

    // Scope to element if requested
    lvt::Element* outputRoot = &tree;
    if (!args.elementId.empty()) {
        outputRoot = find_element(tree, args.elementId);
        if (!outputRoot) {
            fprintf(stderr, "lvt: element '%s' not found\n", args.elementId.c_str());
            return 1;
        }
    }

    // Apply depth limit relative to the output root
    if (args.depth >= 0) {
        lvt::trim_to_depth(*outputRoot, args.depth);
    }

    // Serialize and output tree (unless suppressed by --screenshot without --dump)
    if (args.dump) {
        std::vector<std::string> frameworkNames;
        for (auto& fi : frameworks) {
            if (fi.version.empty())
                frameworkNames.push_back(lvt::framework_to_string(fi.type));
            else
                frameworkNames.push_back(lvt::framework_to_string(fi.type) + " " + fi.version);
        }

        std::string serialized;
        if (args.format == "xml") {
            serialized = lvt::serialize_to_xml(*outputRoot, target.hwnd, target.pid,
                                                target.processName, frameworkNames);
        } else {
            serialized = lvt::serialize_to_json(*outputRoot, target.hwnd, target.pid,
                                                 target.processName, frameworkNames);
        }

        if (args.outputFile.empty()) {
            printf("%s\n", serialized.c_str());
        } else {
            std::ofstream out(args.outputFile);
            if (!out) {
                fprintf(stderr, "lvt: cannot write to '%s'\n", args.outputFile.c_str());
                return 1;
            }
            out << serialized << "\n";
            if (lvt::g_debug)
                fprintf(stderr, "lvt: wrote tree to %s\n", args.outputFile.c_str());
        }
    }

    // Screenshot
    if (!args.screenshotFile.empty()) {
        bool ok = lvt::capture_screenshot(target.hwnd, args.screenshotFile,
                                          &tree, args.elementId);
        if (ok && lvt::g_debug) {
            fprintf(stderr, "lvt: saved screenshot to %s\n", args.screenshotFile.c_str());
        }
    }

    return 0;
}
