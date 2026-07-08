// Unit tests for the LVT Chromium plugin components.
// Tests the DOM JSON format compatibility with plugin_loader's graft_json_node,
// and the native messaging length-prefix protocol.

#include "element.h"
#include "element_key.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <cstring>

#include "plugin_chromium/tab_selection.h"

using json = nlohmann::json;

namespace {

std::string sanitize(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (unsigned char ch : value) {
        if (ch >= 0x20)
            result.push_back(static_cast<char>(ch));
    }
    return result;
}

std::optional<int> safe_double_to_int(double value) {
    if (!std::isfinite(value))
        return std::nullopt;
    if (value < static_cast<double>((std::numeric_limits<int>::min)()) ||
        value > static_cast<double>((std::numeric_limits<int>::max)()))
        return std::nullopt;
    return static_cast<int>(std::round(value));
}

void graft_chromium_json_node(const json& j, lvt::Element& parent, const std::string& framework,
                              double parentOffsetX = 0, double parentOffsetY = 0) {
    lvt::Element el;
    el.framework = framework;
    el.className = sanitize(j.value("type", ""));
    el.text = sanitize(j.value("text", ""));
    auto name = sanitize(j.value("name", ""));
    if (!name.empty())
        el.properties["name"] = name;
    if (el.text.empty())
        el.text = name;

    auto lastDot = el.className.rfind('.');
    el.type = (lastDot != std::string::npos) ? el.className.substr(lastDot + 1) : el.className;

    double ox = j.value("offsetX", 0.0);
    double oy = j.value("offsetY", 0.0);
    double w = j.value("width", 0.0);
    double h = j.value("height", 0.0);
    double absX = std::isfinite(ox) ? parentOffsetX + ox : parentOffsetX;
    double absY = std::isfinite(oy) ? parentOffsetY + oy : parentOffsetY;
    if (w > 0 && h > 0) {
        auto sx = safe_double_to_int(absX);
        auto sy = safe_double_to_int(absY);
        auto sw = safe_double_to_int(w);
        auto sh = safe_double_to_int(h);
        if (sx && sy && sw && sh) {
            el.bounds.x = *sx;
            el.bounds.y = *sy;
            el.bounds.width = *sw;
            el.bounds.height = *sh;
        }
    }

    if (j.contains("properties") && j["properties"].is_object()) {
        for (auto& [key, val] : j["properties"].items()) {
            el.properties[key] = val.is_string() ? val.get<std::string>() : val.dump();
        }
    }

    if (j.contains("children") && j["children"].is_array()) {
        for (auto& child : j["children"]) {
            graft_chromium_json_node(child, el, framework, absX, absY);
        }
    }

    parent.children.push_back(std::move(el));
}

void assign_element_ids(lvt::Element& root) {
    int counter = 0;
    std::vector<lvt::Element*> stack{&root};
    while (!stack.empty()) {
        auto* element = stack.back();
        stack.pop_back();
        element->id = "e" + std::to_string(counter++);
        for (auto it = element->children.rbegin(); it != element->children.rend(); ++it) {
            stack.push_back(&*it);
        }
    }
}

lvt::Element build_chromium_tree(const json& tree) {
    lvt::Element root;
    root.type = "Window";
    root.framework = "win32";
    root.className = "Chrome_WidgetWin_1";

    for (const auto& child : tree) {
        graft_chromium_json_node(child, root, "chromium (Edge)");
    }

    assign_element_ids(root);
    lvt::assign_element_keys(root);
    return root;
}

void collect_keys(const lvt::Element& element, std::vector<std::string>& keys) {
    keys.push_back(element.key);
    for (const auto& child : element.children) {
        collect_keys(child, keys);
    }
}

json read_json_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    EXPECT_TRUE(input.is_open()) << path.string();
    json parsed;
    input >> parsed;
    return parsed;
}

void expect_referenced_file_exists(const std::filesystem::path& extensionDir,
                                   const json& manifest,
                                   const std::string& path) {
    ASSERT_TRUE(manifest.contains(json::json_pointer(path))) << path;
    auto relative = manifest.at(json::json_pointer(path)).get<std::string>();
    EXPECT_TRUE(std::filesystem::exists(extensionDir / relative)) << relative;
}

void validate_extension_package(const std::filesystem::path& extensionDir) {
    auto manifestPath = extensionDir / "manifest.json";
    ASSERT_TRUE(std::filesystem::exists(manifestPath)) << manifestPath.string();
    auto manifest = read_json_file(manifestPath);

    EXPECT_EQ(manifest.value("manifest_version", 0), 3);
    EXPECT_EQ(manifest.value("key", ""),
              "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAg2amT6+n68/syIKGL+QwK0pkveIxgGX+9TTCFLr7RInxjsnh3vZGGIjaowfdPDfxruijJL8LMgHxASOBBjuUCWqBMPQOHw9LCLbULunNY8ahXxjZjocas1UhSQVwf7Hk990XdBzrwyqjE49F3wGR3Wbdvmg5FKHhuiLpDVzRKy6U5pKmyPb4bU6yrVgWefOSFfJdzPCir6vs6RyExy745+SUmwWz0eHCnPpNSdEInZ+LyzDGvkhPkfYXC2A2VfF+cK04cj9i+5+5K8Xnopt9g9/PkdSm+i257jhg5tlVUaekGzFtm8EyCAfETMUna4kYqKXH62u6Fui4nqpAXbH8AQIDAQAB");
    expect_referenced_file_exists(extensionDir, manifest, "/background/service_worker");

    ASSERT_TRUE(manifest.contains("icons"));
    ASSERT_TRUE(manifest["icons"].is_object());
    for (const auto& size : {"16", "48", "128"}) {
        ASSERT_TRUE(manifest["icons"].contains(size)) << size;
        auto relative = manifest["icons"][size].get<std::string>();
        EXPECT_TRUE(std::filesystem::exists(extensionDir / relative)) << relative;
    }
}

} // namespace

// ---- DOM JSON format tests ----
// Verify that the JSON format produced by the extension is compatible
// with the plugin_loader's graft_json_node expectations.

TEST(ChromiumDomJson, BasicElement) {
    // Simulate what the extension produces for a <div id="app" class="container">Hello</div>
    json element = {
        {"type", "DIV"},
        {"text", "Hello"},
        {"offsetX", 10},
        {"offsetY", 20},
        {"width", 800},
        {"height", 600},
        {"properties", {{"id", "app"}, {"class", "container"}}}
    };

    // Verify fields match graft_json_node expectations
    EXPECT_EQ(element.value("type", ""), "DIV");
    EXPECT_EQ(element.value("text", ""), "Hello");
    EXPECT_EQ(element.value("offsetX", 0.0), 10.0);
    EXPECT_EQ(element.value("offsetY", 0.0), 20.0);
    EXPECT_EQ(element.value("width", 0.0), 800.0);
    EXPECT_EQ(element.value("height", 0.0), 600.0);
    EXPECT_TRUE(element.contains("properties"));
    EXPECT_TRUE(element["properties"].is_object());
    EXPECT_EQ(element["properties"]["id"], "app");
}

TEST(ChromiumDomJson, NestedTree) {
    json tree = json::array({
        {
            {"type", "HTML"},
            {"children", json::array({
                {
                    {"type", "HEAD"},
                    {"children", json::array()}
                },
                {
                    {"type", "BODY"},
                    {"offsetX", 0},
                    {"offsetY", 0},
                    {"width", 1920},
                    {"height", 1080},
                    {"children", json::array({
                        {
                            {"type", "DIV"},
                            {"text", "Content"},
                            {"properties", {{"class", "main"}}}
                        }
                    })}
                }
            })}
        }
    });

    ASSERT_TRUE(tree.is_array());
    ASSERT_EQ(tree.size(), 1);
    EXPECT_EQ(tree[0]["type"], "HTML");
    ASSERT_EQ(tree[0]["children"].size(), 2);
    EXPECT_EQ(tree[0]["children"][1]["type"], "BODY");
    EXPECT_EQ(tree[0]["children"][1]["children"][0]["text"], "Content");
}

TEST(ChromiumDomJson, ResponseEnvelope) {
    // The extension wraps the tree in an envelope
    json response = {
        {"type", "domTree"},
        {"requestId", "1"},
        {"url", "https://example.com"},
        {"title", "Example"},
        {"tree", json::array({
            {{"type", "HTML"}, {"children", json::array()}}
        })}
    };

    // Plugin extracts the "tree" field
    ASSERT_TRUE(response.contains("tree"));
    ASSERT_TRUE(response["tree"].is_array());
    json tree = response["tree"];
    EXPECT_EQ(tree.size(), 1);
    EXPECT_EQ(tree[0]["type"], "HTML");
}

TEST(ChromiumDomJson, ErrorResponse) {
    json response = {
        {"type", "error"},
        {"message", "No active tab found"}
    };

    EXPECT_EQ(response["type"], "error");
    EXPECT_EQ(response["message"], "No active tab found");
}

TEST(ChromiumDomJson, EmptyProperties) {
    // Elements with no attributes should not have a properties field
    json element = {
        {"type", "DIV"},
        {"width", 100},
        {"height", 50}
    };

    EXPECT_FALSE(element.contains("properties"));
    // graft_json_node checks: j.contains("properties") && j["properties"].is_object()
    // So missing properties is fine
}

TEST(ChromiumDomJson, ElementWithShadowRoot) {
    // Shadow roots appear as document fragment nodes in the tree
    json element = {
        {"type", "DIV"},
        {"children", json::array({
            {
                {"type", "#document-fragment"},
                {"children", json::array({
                    {{"type", "SLOT"}}
                })}
            }
        })}
    };

    ASSERT_TRUE(element["children"].is_array());
    EXPECT_EQ(element["children"][0]["type"], "#document-fragment");
}

TEST(ChromiumDomJson, DurableKeysAreUniqueAndStable) {
    json tree = json::array({
        {
            {"type", "HTML"},
            {"children", json::array({
                {
                    {"type", "BODY"},
                    {"offsetX", 0},
                    {"offsetY", 0},
                    {"width", 1024},
                    {"height", 768},
                    {"children", json::array({
                        {
                            {"type", "DIV"},
                            {"text", "First"},
                            {"properties", {{"id", "first"}, {"class", "card"}}}
                        },
                        {
                            {"type", "DIV"},
                            {"text", "Second"},
                            {"properties", {{"id", "second"}, {"class", "card"}}}
                        },
                        {
                            {"type", "BUTTON"},
                            {"name", "Submit"},
                            {"properties", {{"name", "submit"}}}
                        }
                    })}
                }
            })}
        }
    });

    auto first = build_chromium_tree(tree);
    auto second = build_chromium_tree(tree);

    std::vector<std::string> firstKeys;
    std::vector<std::string> secondKeys;
    collect_keys(first, firstKeys);
    collect_keys(second, secondKeys);

    ASSERT_EQ(firstKeys.size(), 6u);
    EXPECT_TRUE(std::none_of(firstKeys.begin(), firstKeys.end(), [](const auto& key) {
        return key.empty();
    }));
    EXPECT_EQ(std::set<std::string>(firstKeys.begin(), firstKeys.end()).size(), firstKeys.size());
    EXPECT_EQ(firstKeys, secondKeys);
}

TEST(ChromiumExtension, ManifestReferencesExistingFiles) {
    validate_extension_package(std::filesystem::path(LVT_SOURCE_DIR) /
                               "src" / "plugin_chromium" / "extension");
    validate_extension_package(std::filesystem::path(LVT_CHROMIUM_EXTENSION_OUTPUT_DIR));
}

// ---- Native messaging protocol tests ----

// Encode a native messaging frame: 4-byte LE length + JSON
static std::vector<uint8_t> encode_native_message(const std::string& json_str) {
    uint32_t len = static_cast<uint32_t>(json_str.size());
    std::vector<uint8_t> frame(4 + len);
    memcpy(frame.data(), &len, 4); // Little-endian on x86/x64
    memcpy(frame.data() + 4, json_str.data(), len);
    return frame;
}

// Decode a native messaging frame
static std::string decode_native_message(const std::vector<uint8_t>& frame) {
    if (frame.size() < 4) return {};
    uint32_t len = 0;
    memcpy(&len, frame.data(), 4);
    if (frame.size() < 4 + len) return {};
    return std::string(reinterpret_cast<const char*>(frame.data() + 4), len);
}

TEST(NativeMessaging, EncodeSimple) {
    auto frame = encode_native_message("{\"type\":\"ping\"}");
    ASSERT_GE(frame.size(), 4u);
    uint32_t len = 0;
    memcpy(&len, frame.data(), 4);
    EXPECT_EQ(len, 15u); // strlen of {"type":"ping"}
    EXPECT_EQ(frame.size(), 4u + 15u);
}

TEST(NativeMessaging, RoundTrip) {
    std::string original = "{\"type\":\"getDOM\",\"tabId\":\"active\"}";
    auto frame = encode_native_message(original);
    auto decoded = decode_native_message(frame);
    EXPECT_EQ(decoded, original);
}

TEST(NativeMessaging, EmptyMessage) {
    auto frame = encode_native_message("");
    uint32_t len = 0;
    memcpy(&len, frame.data(), 4);
    EXPECT_EQ(len, 0u);
}

TEST(NativeMessaging, LargeMessage) {
    // Simulate a large DOM tree (1MB)
    std::string large(1024 * 1024, 'x');
    auto frame = encode_native_message(large);
    uint32_t len = 0;
    memcpy(&len, frame.data(), 4);
    EXPECT_EQ(len, 1024u * 1024u);
    auto decoded = decode_native_message(frame);
    EXPECT_EQ(decoded.size(), large.size());
}

TEST(NativeMessaging, TruncatedFrame) {
    std::vector<uint8_t> frame = {0x0A, 0x00, 0x00, 0x00}; // claims 10 bytes, but no payload
    auto decoded = decode_native_message(frame);
    EXPECT_TRUE(decoded.empty());
}

TEST(ChromiumTabSelection, MatchByUrlSubstring) {
    json targets = {
        {"type", "tabs"},
        {"tabs", json::array({
            {{"id", 4}, {"url", "https://example.com/"}, {"title", "Example"}},
            {{"id", 7}, {"url", "https://github.com/asklar/lvt/pull/1"}, {"title", "Pull Request"}}
        })}
    };

    std::string error;
    auto selected = select_chromium_tab_target(targets, "asklar/lvt", error);
    ASSERT_TRUE(selected.has_value()) << error;
    EXPECT_EQ(selected->tab_id, 7);
}

TEST(ChromiumTabSelection, MatchByTitleSubstring) {
    json targets = json::array({
        {{"id", 1}, {"url", "https://example.com/"}, {"title", "Example"}},
        {{"id", 2}, {"url", "https://news.ycombinator.com/"}, {"title", "Hacker News"}}
    });

    std::string error;
    auto selected = select_chromium_tab_target(targets, "title:hacker", error);
    ASSERT_TRUE(selected.has_value()) << error;
    EXPECT_EQ(selected->tab_id, 2);
}

TEST(ChromiumTabSelection, WildcardPattern) {
    json targets = json::array({
        {{"id", 11}, {"url", "https://learn.microsoft.com/windows/apps/"}, {"title", "Docs"}},
        {{"id", 12}, {"url", "https://github.com/"}, {"title", "GitHub"}}
    });

    std::string error;
    auto selected = select_chromium_tab_target(targets, "url:*microsoft.com/windows*", error);
    ASSERT_TRUE(selected.has_value()) << error;
    EXPECT_EQ(selected->tab_id, 11);
}

TEST(ChromiumTabSelection, NoMatchError) {
    json targets = {
        {"tabs", json::array({
            {{"id", 1}, {"url", "https://example.com/"}, {"title", "Example"}}
        })}
    };

    std::string error;
    auto selected = select_chromium_tab_target(targets, "not-present", error);
    EXPECT_FALSE(selected.has_value());
    EXPECT_NE(error.find("No Chromium tab matches 'not-present'"), std::string::npos);
}

TEST(ChromiumTabSelection, IgnoresNonDebuggableTabs) {
    json targets = json::array({
        {{"id", 1}, {"url", "chrome://settings/"}, {"title", "Settings"}},
        {{"id", 2}, {"url", "https://example.com/settings"}, {"title", "Settings"}}
    });

    std::string error;
    auto selected = select_chromium_tab_target(targets, "settings", error);
    ASSERT_TRUE(selected.has_value()) << error;
    EXPECT_EQ(selected->tab_id, 2);
}
