#include "target.h"
#include "framework_detector.h"
#include "tree_builder.h"
#include "json_serializer.h"
#include "screenshot.h"

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
        "\n"
        "Options:\n"
        "  --hwnd <handle>      Target window by HWND (hex, e.g. 0x1A0B3C)\n"
        "  --pid <pid>          Target process by PID (finds main window)\n"
        "  --output <file>      Write JSON to file instead of stdout\n"
        "  --screenshot <file>  Capture annotated screenshot to PNG\n"
        "  --element <id>       Scope to a specific element subtree\n"
        "  --frameworks         Just detect and list frameworks\n"
        "  --depth <n>          Max tree traversal depth (default: unlimited)\n"
        "  --help               Show this help\n"
    );
}

struct Args {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::string outputFile;
    std::string screenshotFile;
    std::string elementId;
    int depth = -1;
    bool frameworksOnly = false;
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
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            args.outputFile = argv[++i];
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            args.screenshotFile = argv[++i];
        } else if (strcmp(argv[i], "--element") == 0 && i + 1 < argc) {
            args.elementId = argv[++i];
        } else if (strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
            args.depth = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--frameworks") == 0) {
            args.frameworksOnly = true;
        } else {
            fprintf(stderr, "lvt: unknown argument '%s'\n", argv[i]);
            print_usage();
            exit(1);
        }
    }
    return args;
}

static const lvt::Element* find_element(const lvt::Element& root, const std::string& id) {
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

    if (!args.hwnd && !args.pid) {
        fprintf(stderr, "lvt: must specify --hwnd or --pid\n");
        return 1;
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
        for (auto f : frameworks) {
            printf("%s\n", lvt::framework_to_string(f).c_str());
        }
        return 0;
    }

    // Build tree
    auto tree = lvt::build_tree(target.hwnd, frameworks, args.depth);

    // Scope to element if requested
    const lvt::Element* outputRoot = &tree;
    if (!args.elementId.empty()) {
        outputRoot = find_element(tree, args.elementId);
        if (!outputRoot) {
            fprintf(stderr, "lvt: element '%s' not found\n", args.elementId.c_str());
            return 1;
        }
    }

    // Serialize to JSON
    std::vector<std::string> frameworkNames;
    for (auto f : frameworks) {
        frameworkNames.push_back(lvt::framework_to_string(f));
    }

    auto json = lvt::serialize_to_json(*outputRoot, target.hwnd, target.pid,
                                       target.processName, frameworkNames);

    // Output JSON
    if (args.outputFile.empty()) {
        printf("%s\n", json.c_str());
    } else {
        std::ofstream out(args.outputFile);
        if (!out) {
            fprintf(stderr, "lvt: cannot write to '%s'\n", args.outputFile.c_str());
            return 1;
        }
        out << json << "\n";
        fprintf(stderr, "lvt: wrote tree to %s\n", args.outputFile.c_str());
    }

    // Screenshot
    if (!args.screenshotFile.empty()) {
        bool ok = lvt::capture_screenshot(target.hwnd, args.screenshotFile,
                                          &tree, args.elementId);
        if (ok) {
            fprintf(stderr, "lvt: saved screenshot to %s\n", args.screenshotFile.c_str());
        }
    }

    return 0;
}
