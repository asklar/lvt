// Unit tests for LVT — pure logic, no live windows required

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include "element.h"
#include "tree_builder.h"
#include "element_key.h"
#include "json_serializer.h"
#include "watch_diff.h"
#include "framework_detector.h"
#include "target.h"
#include "providers/winforms_inject.h"
#include "providers/wpf_inject.h"
#include "providers/uia_provider.h"
#include "providers/connection_registry.h"
#include "providers/framework_connection.h"
#include "providers/managed_connection.h"
#include "providers/native_message.h"
#include "providers/native_property_connection.h"
#include "providers/native_win_event.h"
#include "providers/overlapped_io.h"
#include "providers/xaml_enum_catalog.h"
#include "providers/uia_props.h"
#include "providers/uia_property_adapter.h"
#include "input.h"
#include "providers/uia_actions.h"
#include "tap/xaml_property_filter.h"
#include "tap/bounded_event_queue.h"
#include "tap/xaml_enum_catalog.h"
#include <oleacc.h>
#include <UIAutomation.h>
#include "wil_diagnostics.h"
#include "debug.h"
#include <wil/result.h>
#include <nlohmann/json.hpp>
#include <limits>
#include <string>

using json = nlohmann::json;
using namespace lvt;

// ---- Element ID assignment ----

TEST(AssignElementIds, SingleElement) {
    Element root;
    root.type = "Window";
    assign_element_ids(root);
    EXPECT_EQ(root.id, "e0");
}

TEST(AssignElementIds, DepthFirstOrder) {
    // root -> [a -> [a1, a2], b]
    Element root;
    root.type = "Root";
    Element a, a1, a2, b;
    a.type = "A"; a1.type = "A1"; a2.type = "A2"; b.type = "B";
    a.children = {a1, a2};
    root.children = {a, b};

    assign_element_ids(root);
    EXPECT_EQ(root.id, "e0");
    EXPECT_EQ(root.children[0].id, "e1");       // a
    EXPECT_EQ(root.children[0].children[0].id, "e2"); // a1
    EXPECT_EQ(root.children[0].children[1].id, "e3"); // a2
    EXPECT_EQ(root.children[1].id, "e4");       // b
}

TEST(AssignElementIds, EmptyChildren) {
    Element root;
    root.type = "Root";
    root.children = {};
    assign_element_ids(root);
    EXPECT_EQ(root.id, "e0");
    EXPECT_TRUE(root.children.empty());
}

TEST(AssignElementIds, DeepTree) {
    // Chain: root -> c1 -> c2 -> c3
    Element root, c1, c2, c3;
    c3.type = "Leaf";
    c2.type = "Mid"; c2.children = {c3};
    c1.type = "Mid"; c1.children = {c2};
    root.type = "Root"; root.children = {c1};
    assign_element_ids(root);
    EXPECT_EQ(root.id, "e0");
    EXPECT_EQ(root.children[0].id, "e1");
    EXPECT_EQ(root.children[0].children[0].id, "e2");
    EXPECT_EQ(root.children[0].children[0].children[0].id, "e3");
}

// ---- framework_to_string ----

TEST(FrameworkToString, AllFrameworks) {
    EXPECT_EQ(framework_to_string(Framework::Win32), "win32");
    EXPECT_EQ(framework_to_string(Framework::ComCtl), "comctl");
    EXPECT_EQ(framework_to_string(Framework::Xaml), "xaml");
    EXPECT_EQ(framework_to_string(Framework::WinUI3), "winui3");
    EXPECT_EQ(framework_to_string(Framework::Wpf), "wpf");
    EXPECT_EQ(framework_to_string(Framework::WinForms), "winforms");
    EXPECT_EQ(framework_to_string(Framework::Plugin), "plugin");
}

TEST(FrameworkDisplayName, BuiltInFramework) {
    FrameworkInfo fi{Framework::Win32, "", ""};
    EXPECT_EQ(framework_display_name(fi), "win32");
}

TEST(FrameworkDisplayName, PluginFramework) {
    FrameworkInfo fi{Framework::Plugin, "14", "dui"};
    EXPECT_EQ(framework_display_name(fi), "dui");
}

TEST(FrameworkDisplayName, BuiltInWithName) {
    FrameworkInfo fi{Framework::ComCtl, "6.10", "comctl"};
    EXPECT_EQ(framework_display_name(fi), "comctl");
}

// ---- JSON serialization ----

static Element make_test_tree() {
    Element root;
    root.type = "Window";
    root.framework = "win32";
    root.className = "MyWindow";
    root.text = "Hello";
    root.bounds = {100, 200, 800, 600};
    root.properties["visible"] = "true";

    Element child;
    child.type = "Button";
    child.framework = "win32";
    child.className = "Button";
    child.text = "OK";
    child.bounds = {110, 210, 80, 30};
    root.children.push_back(child);

    assign_element_ids(root);
    assign_element_keys(root);
    return root;
}

TEST(JsonSerializer, BasicStructure) {
    auto root = make_test_tree();
    auto result = serialize_to_json(root, (HWND)0x1234, 42, "test.exe", {"win32"});
    auto j = json::parse(result);

    EXPECT_TRUE(j.contains("target"));
    EXPECT_TRUE(j.contains("frameworks"));
    EXPECT_TRUE(j.contains("root"));
    EXPECT_EQ(j["target"]["pid"], 42);
    EXPECT_EQ(j["target"]["processName"], "test.exe");
    EXPECT_EQ(j["frameworks"], json({"win32"}));
}

TEST(JsonSerializer, ElementFields) {
    auto root = make_test_tree();
    auto result = serialize_to_json(root, (HWND)0x1234, 42, "test.exe", {"win32"});
    auto j = json::parse(result);

    auto& r = j["root"];
    EXPECT_EQ(r["id"], "e0");
    EXPECT_FALSE(r["key"].get<std::string>().empty());
    EXPECT_EQ(r["type"], "Window");
    EXPECT_EQ(r["framework"], "win32");
    EXPECT_EQ(r["className"], "MyWindow");
    EXPECT_EQ(r["text"], "Hello");
    EXPECT_EQ(r["bounds"]["x"], 100);
    EXPECT_EQ(r["bounds"]["y"], 200);
    EXPECT_EQ(r["bounds"]["width"], 800);
    EXPECT_EQ(r["bounds"]["height"], 600);
    EXPECT_EQ(r["properties"]["visible"], "true");
}

TEST(JsonSerializer, ChildElements) {
    auto root = make_test_tree();
    auto result = serialize_to_json(root, (HWND)0x1234, 42, "test.exe", {"win32"});
    auto j = json::parse(result);

    EXPECT_TRUE(j["root"].contains("children"));
    EXPECT_EQ(j["root"]["children"].size(), 1);
    auto& child = j["root"]["children"][0];
    EXPECT_EQ(child["id"], "e1");
    EXPECT_EQ(child["type"], "Button");
    EXPECT_EQ(child["text"], "OK");
}

TEST(JsonSerializer, NativeHandleIsPreservedAsHex) {
    auto root = make_test_tree();
    root.nativeHandle = 0x1234ABCD;
    auto result = serialize_to_json(root, (HWND)0x1234, 42, "test.exe", {"win32"});
    auto j = json::parse(result);

    EXPECT_EQ(j["root"]["nativeHandle"], "0x1234ABCD");
}

TEST(JsonSerializer, ControlCharsSanitized) {
    Element root;
    root.type = "Win\x01" "dow";  // embedded control char
    root.framework = "win32";
    root.className = "My\x02" "Class";
    root.text = "He\x03llo";
    assign_element_ids(root);

    auto result = serialize_to_json(root, nullptr, 0, "test.exe", {});
    auto j = json::parse(result);
    EXPECT_EQ(j["root"]["type"], "Window");      // \x01 stripped
    EXPECT_EQ(j["root"]["className"], "MyClass"); // \x02 stripped
    EXPECT_EQ(j["root"]["text"], "Hello");        // \x03 stripped
}

TEST(JsonSerializer, NoChildrenKey) {
    Element root;
    root.type = "Leaf";
    root.framework = "win32";
    assign_element_ids(root);

    auto result = serialize_to_json(root, nullptr, 0, "test.exe", {});
    auto j = json::parse(result);
    EXPECT_FALSE(j["root"].contains("children"));
}

TEST(JsonSerializer, EmptyOptionalFields) {
    Element root;
    root.type = "Window";
    root.framework = "win32";
    // className and text are empty
    assign_element_ids(root);

    auto result = serialize_to_json(root, nullptr, 0, "test.exe", {});
    auto j = json::parse(result);
    EXPECT_FALSE(j["root"].contains("className"));
    EXPECT_FALSE(j["root"].contains("text"));
}

TEST(JsonSerializer, MultipleFrameworks) {
    auto root = make_test_tree();
    auto result = serialize_to_json(root, nullptr, 0, "test.exe", {"win32", "comctl", "winui3"});
    auto j = json::parse(result);
    EXPECT_EQ(j["frameworks"].size(), 3);
    EXPECT_EQ(j["frameworks"][0], "win32");
    EXPECT_EQ(j["frameworks"][1], "comctl");
    EXPECT_EQ(j["frameworks"][2], "winui3");
}

// ---- XML serialization ----

TEST(XmlSerializer, BasicStructure) {
    auto root = make_test_tree();
    auto result = serialize_to_xml(root, (HWND)0x1234, 42, "test.exe", {"win32"});

    EXPECT_NE(result.find("<LiveVisualTree"), std::string::npos);
    EXPECT_NE(result.find("</LiveVisualTree>"), std::string::npos);
    EXPECT_NE(result.find("pid=\"42\""), std::string::npos);
    EXPECT_NE(result.find("process=\"test.exe\""), std::string::npos);
    EXPECT_NE(result.find("frameworks=\"win32\""), std::string::npos);
}

TEST(XmlSerializer, ElementAttributes) {
    auto root = make_test_tree();
    auto result = serialize_to_xml(root, nullptr, 0, "test.exe", {});

    EXPECT_NE(result.find("<Window"), std::string::npos);
    EXPECT_NE(result.find("id=\"e0\""), std::string::npos);
    EXPECT_NE(result.find("framework=\"win32\""), std::string::npos);
    EXPECT_NE(result.find("text=\"Hello\""), std::string::npos);
    EXPECT_NE(result.find("bounds=\"100,200,800,600\""), std::string::npos);
}

TEST(XmlSerializer, ChildNesting) {
    auto root = make_test_tree();
    auto result = serialize_to_xml(root, nullptr, 0, "test.exe", {});

    EXPECT_NE(result.find("<Button"), std::string::npos);
    EXPECT_NE(result.find("</Window>"), std::string::npos);
}

TEST(XmlSerializer, SelfClosingLeaf) {
    Element root;
    root.type = "Leaf";
    root.framework = "test";
    assign_element_ids(root);
    auto result = serialize_to_xml(root, nullptr, 0, "test.exe", {});

    EXPECT_NE(result.find("<Leaf"), std::string::npos);
    EXPECT_NE(result.find("/>"), std::string::npos);
    EXPECT_EQ(result.find("</Leaf>"), std::string::npos);
}

TEST(XmlSerializer, SpecialCharsEscaped) {
    Element root;
    root.type = "Window";
    root.framework = "win32";
    root.text = "File & <Edit>";
    assign_element_ids(root);
    auto result = serialize_to_xml(root, nullptr, 0, "test.exe", {});

    EXPECT_NE(result.find("&amp;"), std::string::npos);
    EXPECT_NE(result.find("&lt;"), std::string::npos);
    EXPECT_NE(result.find("&gt;"), std::string::npos);
}

TEST(XmlSerializer, InvalidTagNameFallback) {
    Element root;
    root.type = "123Invalid";  // starts with digit
    root.framework = "test";
    assign_element_ids(root);
    auto result = serialize_to_xml(root, nullptr, 0, "test.exe", {});

    // Should fall back to "Element" tag
    EXPECT_NE(result.find("<Element"), std::string::npos);
}

TEST(XmlSerializer, ControlCharsStripped) {
    Element root;
    root.type = "Win\x01" "dow";
    root.framework = "test";
    assign_element_ids(root);
    auto result = serialize_to_xml(root, nullptr, 0, "test.exe", {});

    EXPECT_NE(result.find("<Window"), std::string::npos);
}

TEST(XmlSerializer, ZeroBoundsOmitted) {
    Element root;
    root.type = "Window";
    root.framework = "test";
    root.bounds = {0, 0, 0, 0};
    assign_element_ids(root);
    auto result = serialize_to_xml(root, nullptr, 0, "test.exe", {});

    EXPECT_EQ(result.find("bounds="), std::string::npos);
}

TEST(XmlSerializer, PropertiesAsAttributes) {
    Element root;
    root.type = "Window";
    root.framework = "test";
    root.properties["visible"] = "true";
    root.properties["style"] = "WS_OVERLAPPED";
    assign_element_ids(root);
    auto result = serialize_to_xml(root, nullptr, 0, "test.exe", {});

    EXPECT_NE(result.find("visible=\"true\""), std::string::npos);
    EXPECT_NE(result.find("style=\"WS_OVERLAPPED\""), std::string::npos);
}

TEST(XmlSerializer, ClassNameOmittedWhenSameAsType) {
    Element root;
    root.type = "Button";
    root.className = "Button";  // same as type
    root.framework = "test";
    assign_element_ids(root);
    auto result = serialize_to_xml(root, nullptr, 0, "test.exe", {});

    EXPECT_EQ(result.find("className="), std::string::npos);
}

TEST(XmlSerializer, ClassNameShownWhenDifferent) {
    Element root;
    root.type = "Button";
    root.className = "Win32Button";  // different
    root.framework = "test";
    assign_element_ids(root);
    auto result = serialize_to_xml(root, nullptr, 0, "test.exe", {});

    EXPECT_NE(result.find("className=\"Win32Button\""), std::string::npos);
}

// ---- Bounds struct ----

TEST(Bounds, DefaultZero) {
    Bounds b;
    EXPECT_EQ(b.x, 0);
    EXPECT_EQ(b.y, 0);
    EXPECT_EQ(b.width, 0);
    EXPECT_EQ(b.height, 0);
}

// ---- Element struct ----

TEST(Element, DefaultValues) {
    Element el;
    EXPECT_TRUE(el.id.empty());
    EXPECT_TRUE(el.key.empty());
    EXPECT_TRUE(el.type.empty());
    EXPECT_TRUE(el.framework.empty());
    EXPECT_TRUE(el.className.empty());
    EXPECT_TRUE(el.text.empty());
    EXPECT_TRUE(el.properties.empty());
    EXPECT_TRUE(el.children.empty());
    EXPECT_EQ(el.nativeHandle, 0u);
}

namespace {

class SnapshotLifetimeConnection final : public IFrameworkConnection {
public:
    explicit SnapshotLifetimeConnection(std::shared_ptr<int> destroyed)
        : destroyed_(std::move(destroyed)) {}
    ~SnapshotLifetimeConnection() override { ++*destroyed_; }

    bool get_tree(Element&, bool, const std::string& = {}) override { return true; }
    std::vector<ConnectionEvent> poll_events() override { return {}; }
    bool is_alive() const override { return true; }

private:
    std::shared_ptr<int> destroyed_;
};

class ReplaceableConnection final : public IFrameworkConnection {
public:
    bool get_tree(Element&, bool, const std::string& = {}) override {
        return true;
    }
    std::vector<ConnectionEvent> poll_events() override { return {}; }
    bool is_alive() const override { return alive; }

    bool alive = true;
};

} // namespace

TEST(ConnectionHandle, SharedSnapshotOutlivesRegistryHandle) {
    auto destroyed = std::make_shared<int>(0);
    auto handle = ConnectionRegistry::instance().acquire(
        GetCurrentProcessId(), nullptr, "unit-snapshot-lifetime",
        [destroyed](HWND, DWORD) {
            return std::make_shared<SnapshotLifetimeConnection>(destroyed);
        });
    ASSERT_TRUE(handle);

    auto snapshot = handle.shared();
    handle.reset();
    EXPECT_EQ(*destroyed, 0);

    snapshot.reset();
    EXPECT_EQ(*destroyed, 1);
}

TEST(ConnectionHandle, StaleGenerationCannotReleaseReplacement) {
    const DWORD pid = GetCurrentProcessId();
    const std::string label = "unit-registry-generation";
    auto oldConnection = std::make_shared<ReplaceableConnection>();
    auto oldFirst = ConnectionRegistry::instance().acquire(
        pid, nullptr, label,
        [oldConnection](HWND, DWORD) { return oldConnection; });
    auto oldSecond = ConnectionRegistry::instance().acquire(
        pid, nullptr, label,
        [](HWND, DWORD) -> std::shared_ptr<IFrameworkConnection> {
            ADD_FAILURE() << "the second old holder should reuse the entry";
            return nullptr;
        });
    ASSERT_TRUE(oldFirst);
    ASSERT_TRUE(oldSecond);

    oldConnection->alive = false;
    auto replacement = std::make_shared<ReplaceableConnection>();
    auto fresh = ConnectionRegistry::instance().acquire(
        pid, nullptr, label,
        [replacement](HWND, DWORD) { return replacement; });
    ASSERT_TRUE(fresh);
    ASSERT_EQ(fresh.get(), replacement.get());

    oldFirst.reset();
    oldSecond.reset();

    int replacementFactories = 0;
    auto freshSecond = ConnectionRegistry::instance().acquire(
        pid, nullptr, label,
        [&replacementFactories](HWND, DWORD)
            -> std::shared_ptr<IFrameworkConnection> {
            ++replacementFactories;
            return std::make_shared<ReplaceableConnection>();
        });
    ASSERT_TRUE(freshSecond);
    EXPECT_EQ(replacementFactories, 0);
    EXPECT_EQ(freshSecond.get(), replacement.get());

    fresh.reset();
    freshSecond.reset();
}

// ---- Durable element keys and lookup ----

static Element key_el(const std::string& type, const std::string& name = "",
                      uintptr_t hwnd = 0) {
    Element el;
    el.type = type;
    el.framework = "win32";
    el.className = type;
    el.nativeHandle = hwnd;
    if (!name.empty())
        el.properties["Name"] = name;
    return el;
}

TEST(ElementKeys, UniqueTypeUnaffectedByEarlierSiblingInsertion) {
    Element root = key_el("Window");
    root.children.push_back(key_el("Button"));
    assign_element_keys(root);
    auto buttonKey = root.children[0].key;

    root.children.insert(root.children.begin(), key_el("Edit"));
    assign_element_keys(root);

    EXPECT_EQ(root.children[1].key, buttonKey);
}

TEST(ElementKeys, StableNameUnaffectedByEarlierDuplicateSiblingInsertion) {
    Element root = key_el("Window");
    root.children.push_back(key_el("Button", "Save"));
    assign_element_keys(root);
    auto saveKey = root.children[0].key;

    root.children.insert(root.children.begin(), key_el("Button", "Cancel"));
    assign_element_keys(root);

    EXPECT_EQ(root.children[1].key, saveKey);
    EXPECT_NE(root.children[0].key, root.children[1].key);
}

TEST(ElementKeys, DistinctNamesStayTiedAfterSwap) {
    Element root = key_el("Window");
    root.children.push_back(key_el("Button", "Save"));
    root.children.push_back(key_el("Button", "Cancel"));
    assign_element_keys(root);
    auto saveKey = root.children[0].key;
    auto cancelKey = root.children[1].key;

    std::swap(root.children[0], root.children[1]);
    assign_element_keys(root);

    EXPECT_EQ(root.children[0].properties["Name"], "Cancel");
    EXPECT_EQ(root.children[0].key, cancelKey);
    EXPECT_EQ(root.children[1].properties["Name"], "Save");
    EXPECT_EQ(root.children[1].key, saveKey);
}

TEST(ElementKeys, NativeHandlesStayTiedAfterSwap) {
    Element root = key_el("Window");
    root.children.push_back(key_el("Button", "", 0x1111));
    root.children.push_back(key_el("Button", "", 0x2222));
    assign_element_keys(root);
    auto firstKey = root.children[0].key;
    auto secondKey = root.children[1].key;

    std::swap(root.children[0], root.children[1]);
    assign_element_keys(root);

    EXPECT_EQ(root.children[0].nativeHandle, 0x2222u);
    EXPECT_EQ(root.children[0].key, secondKey);
    EXPECT_EQ(root.children[1].nativeHandle, 0x1111u);
    EXPECT_EQ(root.children[1].key, firstKey);
}

TEST(ElementKeys, XamlInstanceHandlesUseCompactProcessWideKeys) {
    Element root = key_el("Window");

    Element panel;
    panel.type = "Grid";
    panel.className = "Microsoft.UI.Xaml.Controls.Grid";
    panel.framework = "winui3";
    panel.providerHandle = UINT64_C(0x123456789ABC);

    Element button;
    button.type = "Button";
    button.className = "Microsoft.UI.Xaml.Controls.Button";
    button.framework = "winui3";
    button.providerHandle = UINT64_C(0xFEDCBA987654);
    panel.children.push_back(std::move(button));
    root.children.push_back(std::move(panel));

    assign_element_keys(root);

    EXPECT_EQ(root.children[0].key, "winui3:0x123456789ABC");
    EXPECT_EQ(root.children[0].children[0].key, "winui3:0xFEDCBA987654");
    EXPECT_EQ(root.children[0].children[0].key.find(root.children[0].key),
              std::string::npos);
}

TEST(ElementKeys, ProcessWideProviderIdentityIsExplicitOptIn) {
    Element xaml;
    xaml.framework = "xaml";
    xaml.providerHandle = 0x101;
    EXPECT_TRUE(has_process_wide_provider_identity(xaml));

    Element winui = xaml;
    winui.framework = "winui3";
    EXPECT_TRUE(has_process_wide_provider_identity(winui));

    Element wpf = xaml;
    wpf.framework = "wpf";
    EXPECT_FALSE(has_process_wide_provider_identity(wpf));

    xaml.providerHandle = 0;
    EXPECT_FALSE(has_process_wide_provider_identity(xaml));
}

TEST(ElementKeys, ManagedFrameworkHandlesUseCompactSessionKeys) {
    Element root = key_el("Window");

    Element wpfButton;
    wpfButton.type = "Button";
    wpfButton.className = "System.Windows.Controls.Button";
    wpfButton.framework = "wpf";
    wpfButton.providerHandle = 0x1234;

    Element winFormsButton;
    winFormsButton.type = "Button";
    winFormsButton.className = "System.Windows.Forms.Button";
    winFormsButton.framework = "winforms";
    winFormsButton.providerHandle = 0x5678;

    root.children.push_back(std::move(wpfButton));
    root.children.push_back(std::move(winFormsButton));
    assign_element_keys(root);

    EXPECT_EQ(root.children[0].key, "wpf:0x1234");
    EXPECT_EQ(root.children[1].key, "winforms:0x5678");
}

TEST(ElementKeys, WinFormsManagedIdentitySurvivesHwndRecreation) {
    Element original;
    original.type = "Button";
    original.className = "System.Windows.Forms.Button";
    original.framework = "winforms";
    original.providerHandle = 0x1234;
    original.nativeHandle = 0x100;
    assign_element_keys(original);

    Element recreated = original;
    recreated.nativeHandle = 0x200;
    assign_element_keys(recreated);
    EXPECT_EQ(recreated.key, original.key);

    Element reusedHwnd = original;
    reusedHwnd.providerHandle = 0x5678;
    reusedHwnd.nativeHandle = 0x100;
    assign_element_keys(reusedHwnd);
    EXPECT_NE(reusedHwnd.key, original.key);
}

TEST(ElementKeys, NativePropertyHandlesUseCompactProviderKeys) {
    Element root;
    root.type = "Window";
    root.className = "FixtureWindow";
    root.framework = "win32";
    root.nativeHandle = 0x1234;
    root.providerHandle = UINT64_C(0x1234);

    Element item;
    item.type = "ListViewItem";
    item.framework = "comctl";
    item.providerHandle = UINT64_C(0x8000000000000042);
    root.children.push_back(std::move(item));

    assign_element_keys(root);

    EXPECT_EQ(root.key, "win32:0x1234");
    EXPECT_EQ(root.children[0].key, "comctl:0x8000000000000042");
}

TEST(ElementKeys, ParsesOnlyCompactXamlInstanceKeys) {
    CompactXamlKey key;
    std::string error;
    ASSERT_TRUE(parse_compact_xaml_key("xaml:0x123ABC", key, error));
    EXPECT_EQ(key.framework, "xaml");
    EXPECT_EQ(key.handle, 0x123ABCu);

    ASSERT_TRUE(parse_compact_xaml_key("winui3:0xFEDCBA987654", key, error));
    EXPECT_EQ(key.framework, "winui3");
    EXPECT_EQ(key.handle, UINT64_C(0xFEDCBA987654));

    ASSERT_TRUE(parse_compact_xaml_key(
        "winui3:0xFEDCBA9876543210", key, error));
    EXPECT_EQ(key.handle, UINT64_C(0xFEDCBA9876543210));

    EXPECT_FALSE(parse_compact_xaml_key("win32|Window/winui3|Button", key, error));
    EXPECT_NE(error.find("compact XAML/WinUI3"), std::string::npos);
    EXPECT_FALSE(parse_compact_xaml_key("winui3:0xZZ", key, error));
    EXPECT_FALSE(parse_compact_xaml_key("xaml:0x0", key, error));
}

TEST(ElementKeys, ProviderHandlesRemain64BitOnEveryArchitecture) {
    static_assert(sizeof(decltype(Element::providerHandle)) == sizeof(uint64_t));
    static_assert(sizeof(decltype(CompactXamlKey::handle)) == sizeof(uint64_t));
    static_assert(sizeof(decltype(ConnectionEvent::handle)) == sizeof(uint64_t));
}

TEST(ElementLookup, ResolvesByIdAndDurableKey) {
    Element root = key_el("Window");
    root.children.push_back(key_el("Button", "Save"));
    assign_element_ids(root);
    assign_element_keys(root);
    auto key = root.children[0].key;

    EXPECT_EQ(find_element_by_ref(root, "e1"), &root.children[0]);
    EXPECT_EQ(find_element_by_ref(root, key), &root.children[0]);
    EXPECT_EQ(find_element_by_ref(root, "missing"), nullptr);
}

TEST(ElementLookup, GetElementPropertyBuiltinsAndDynamic) {
    Element el = key_el("Button", "Save", 0x1234);
    el.id = "e7";
    el.key = "stable-key";
    el.text = "Click";
    el.bounds = {1, 2, 3, 4};
    el.properties["custom"] = "value";

    EXPECT_EQ(get_element_property(el, "id"), "e7");
    EXPECT_EQ(get_element_property(el, "key"), "stable-key");
    EXPECT_EQ(get_element_property(el, "type"), "Button");
    EXPECT_EQ(get_element_property(el, "framework"), "win32");
    EXPECT_EQ(get_element_property(el, "className"), "Button");
    EXPECT_EQ(get_element_property(el, "text"), "Click");
    EXPECT_EQ(get_element_property(el, "bounds"), "1,2,3,4");
    EXPECT_EQ(get_element_property(el, "Name"), "Save");
    EXPECT_EQ(get_element_property(el, "custom"), "value");
    EXPECT_FALSE(get_element_property(el, "missing").has_value());
}

TEST(Element, TreeConstruction) {
    Element root;
    root.type = "Root";
    Element child1, child2;
    child1.type = "Child1";
    child2.type = "Child2";
    root.children = {child1, child2};

    EXPECT_EQ(root.children.size(), 2);
    EXPECT_EQ(root.children[0].type, "Child1");
    EXPECT_EQ(root.children[1].type, "Child2");
}

// ---- WinForms enrichment ----

TEST(WinFormsEnrichment, AppliesManagedPropertiesByHwnd) {
    Element root;
    root.type = "Window";
    root.framework = "win32";
    root.nativeHandle = 0x100;

    Element child;
    child.type = "Button";
    child.framework = "win32";
    child.nativeHandle = 0x200;
    child.properties["hwnd"] = "0x00000200";
    root.children.push_back(child);

    auto ok = apply_winforms_control_json(root,
        R"([{"hwnd":"100","type":"System.Windows.Forms.Form","name":"MainForm","children":[)"
        R"({"hwnd":"200","type":"System.Windows.Forms.Button","name":"okButton","text":"OK","enabled":true,"autoSize":false})"
        R"(]}])");

    ASSERT_TRUE(ok);
    EXPECT_EQ(root.framework, "winforms");
    EXPECT_EQ(root.type, "Form");
    EXPECT_EQ(root.properties["winforms.type"], "System.Windows.Forms.Form");
    EXPECT_EQ(root.properties["name"], "MainForm");

    auto& enriched = root.children[0];
    EXPECT_EQ(enriched.framework, "winforms");
    EXPECT_EQ(enriched.type, "Button");
    EXPECT_EQ(enriched.properties["winforms.type"], "System.Windows.Forms.Button");
    EXPECT_EQ(enriched.properties["name"], "okButton");
    EXPECT_EQ(enriched.properties["winforms.text"], "OK");
    EXPECT_EQ(enriched.properties["winforms.enabled"], "true");
    EXPECT_EQ(enriched.properties["autoSize"], "false");
}

TEST(WinFormsEnrichment, InvalidJsonDoesNotModifyTree) {
    Element root;
    root.type = "Window";
    root.framework = "win32";
    root.nativeHandle = 0x100;

    EXPECT_FALSE(apply_winforms_control_json(root, "{not json"));
    EXPECT_EQ(root.framework, "win32");
    EXPECT_EQ(root.type, "Window");
}

TEST(WinFormsEnrichment, PreservesManagedIdentityForControlsWithoutHwnd) {
    Element root;
    root.type = "Window";
    root.framework = "win32";
    root.nativeHandle = 0x100;

    ASSERT_TRUE(apply_winforms_control_json(
        root,
        R"([{"hwnd":"100","managedHandle":1,"type":"System.Windows.Forms.Form","children":[)"
        R"({"managedHandle":2,"type":"System.Windows.Forms.Control","name":"windowless"})"
        R"(]}])"));

    ASSERT_EQ(root.children.size(), 1u);
    const auto& child = root.children[0];
    EXPECT_EQ(child.framework, "winforms");
    EXPECT_EQ(child.nativeHandle, 0u);
    EXPECT_EQ(child.providerHandle, 2u);
    EXPECT_EQ(child.properties.at("managedHandle"), "0x2");
    EXPECT_EQ(child.properties.at("handleKind"), "managed");
}

TEST(WinFormsEnrichment, HwndIndexSurvivesHandlelessChildInsertion) {
    Element root;
    root.type = "Window";
    root.framework = "win32";
    root.nativeHandle = 0x100;

    Element first;
    first.type = "Window";
    first.framework = "win32";
    first.nativeHandle = 0x200;
    Element later;
    later.type = "Window";
    later.framework = "win32";
    later.nativeHandle = 0x300;
    root.children = {first, later};
    root.children.shrink_to_fit();

    ASSERT_TRUE(apply_winforms_control_json(
        root,
        R"([{"hwnd":"100","managedHandle":1,"type":"System.Windows.Forms.Form","children":[)"
        R"({"hwnd":"200","managedHandle":2,"type":"System.Windows.Forms.Button","name":"first"},)"
        R"({"managedHandle":3,"type":"System.Windows.Forms.Control","name":"handleless"},)"
        R"({"hwnd":"300","managedHandle":4,"type":"System.Windows.Forms.Button","name":"later"})"
        R"(]}])"));

    ASSERT_EQ(root.children.size(), 3u);
    EXPECT_EQ(root.children[0].properties.at("name"), "first");
    EXPECT_EQ(root.children[0].providerHandle, 2u);
    EXPECT_EQ(root.children[1].properties.at("name"), "later");
    EXPECT_EQ(root.children[1].providerHandle, 4u);
    EXPECT_EQ(root.children[2].properties.at("name"), "handleless");
    EXPECT_EQ(root.children[2].providerHandle, 3u);
}

namespace {

class FailingManagedTreeConnection final : public IFrameworkConnection {
public:
    bool get_tree(Element&, bool, const std::string& = {}) override {
        return false;
    }
    std::vector<ConnectionEvent> poll_events() override { return {}; }
    bool is_alive() const override { return true; }
};

} // namespace

TEST(ManagedFrameworkCompleteness, FailedRefreshIsExplicit) {
    FailingManagedTreeConnection connection;
    std::vector<FrameworkInfo> frameworks{
        {Framework::Wpf, "", "wpf"},
        {Framework::WinForms, "", "winforms"},
    };
    auto tree = build_tree(
        nullptr, GetCurrentProcessId(), frameworks, -1, {}, false,
        [&connection](const std::string&) -> IFrameworkConnection* {
            return &connection;
        });

    EXPECT_TRUE(framework_refresh_incomplete(tree, "wpf"));
    EXPECT_TRUE(framework_refresh_incomplete(tree, "winforms"));
    EXPECT_TRUE(has_incomplete_framework_refresh(tree));
}

TEST(WpfTreeJson, ZeroSizeIsMarkedRatherThanSilentlyOmitted) {
    // Before the fix, w<=0 || h<=0 meant no width/height/offset/zeroSize at
    // all: indistinguishable from PresentationSource throwing or the element
    // never having been laid out. zeroSize says explicitly "measured, and it
    // really is zero".
    auto roots = lvt::wpf_parse_tree_json(
        R"([{"type":"System.Windows.Controls.Border","zeroSize":true}])", "wpf");
    ASSERT_TRUE(roots.has_value());
    ASSERT_EQ(roots->size(), 1u);
    EXPECT_EQ((*roots)[0].properties["zeroSize"], "true");
    EXPECT_EQ((*roots)[0].bounds.width, 0);
    EXPECT_EQ((*roots)[0].bounds.height, 0);
}

TEST(WpfTreeJson, PreservesManagedIdentity) {
    auto roots = lvt::wpf_parse_tree_json(
        R"([{"managedHandle":42,"type":"System.Windows.Controls.Button"}])", "wpf");
    ASSERT_TRUE(roots.has_value());
    ASSERT_EQ(roots->size(), 1u);
    EXPECT_EQ((*roots)[0].nativeHandle, 0u);
    EXPECT_EQ((*roots)[0].providerHandle, 42u);
    EXPECT_EQ((*roots)[0].properties.at("managedHandle"), "0x2A");
    EXPECT_EQ((*roots)[0].properties.at("handleKind"), "managed");
}

TEST(WpfTreeJson, NonZeroBoundsAreGraftedNormally) {
    auto roots = lvt::wpf_parse_tree_json(
        R"([{"type":"System.Windows.Controls.Button","width":100.0,"height":24.0,)"
        R"("offsetX":10.0,"offsetY":20.0}])", "wpf");
    ASSERT_TRUE(roots.has_value());
    ASSERT_EQ(roots->size(), 1u);
    EXPECT_EQ((*roots)[0].bounds.width, 100);
    EXPECT_EQ((*roots)[0].bounds.height, 24);
    EXPECT_EQ((*roots)[0].bounds.x, 10);
    EXPECT_EQ((*roots)[0].bounds.y, 20);
    EXPECT_TRUE((*roots)[0].properties["zeroSize"].empty());
}

TEST(WpfTreeJson, HiddenAndCollapsedAreDistinguishedByWpfVisibility) {
    // "visible":false stays the same across both, matching every other
    // provider's generic click-safety signal. wpf.visibility is what
    // actually answers "does this reserve layout space or not".
    auto hidden = lvt::wpf_parse_tree_json(
        R"([{"type":"TextBox","visible":false,"wpf.visibility":"Hidden"}])", "wpf");
    auto collapsed = lvt::wpf_parse_tree_json(
        R"([{"type":"TextBox","visible":false,"wpf.visibility":"Collapsed"}])", "wpf");
    ASSERT_TRUE(hidden.has_value());
    ASSERT_TRUE(collapsed.has_value());
    ASSERT_EQ(hidden->size(), 1u);
    ASSERT_EQ(collapsed->size(), 1u);
    EXPECT_EQ((*hidden)[0].properties["visible"], "false");
    EXPECT_EQ((*collapsed)[0].properties["visible"], "false");
    EXPECT_EQ((*hidden)[0].properties["wpf.visibility"], "Hidden");
    EXPECT_EQ((*collapsed)[0].properties["wpf.visibility"], "Collapsed");
    EXPECT_NE((*hidden)[0].properties["wpf.visibility"], (*collapsed)[0].properties["wpf.visibility"]);
}

TEST(WpfTreeJson, VisibleElementsCarryNoVisibilityOverride) {
    auto roots = lvt::wpf_parse_tree_json(R"([{"type":"TextBox"}])", "wpf");
    ASSERT_TRUE(roots.has_value());
    ASSERT_EQ(roots->size(), 1u);
    EXPECT_TRUE((*roots)[0].properties["visible"].empty());
    EXPECT_TRUE((*roots)[0].properties["wpf.visibility"].empty());
}

TEST(WpfTreeJson, EmptyTextIsPreservedRatherThanFallingBackToTheName) {
    // graft_json_node falls back el.text = name only when the walker never
    // sent a "text" key at all. When it did send one - even "" - that is a
    // real answer (an empty TextBox is not the same as an unnamed Border)
    // and must survive as the element's text, not be overwritten by name.
    auto roots = lvt::wpf_parse_tree_json(
        R"([{"type":"TextBox","name":"searchBox","text":""}])", "wpf");
    ASSERT_TRUE(roots.has_value());
    ASSERT_EQ(roots->size(), 1u);
    EXPECT_EQ((*roots)[0].properties["name"], "searchBox");
    EXPECT_EQ((*roots)[0].text, "");
}

TEST(WpfTreeJson, MissingTextKeyFallsBackToName) {
    auto roots = lvt::wpf_parse_tree_json(
        R"([{"type":"Border","name":"outerBorder"}])", "wpf");
    ASSERT_TRUE(roots.has_value());
    ASSERT_EQ(roots->size(), 1u);
    EXPECT_EQ((*roots)[0].text, "outerBorder");
}

TEST(WpfTreeJson, InvalidJsonIsDistinctFromAValidEmptyTree) {
    // Before this fix, a parse failure and "[]" (a real, empty tree) were
    // both represented as an empty vector - indistinguishable from each
    // other, and from the caller's point of view, from success. nullopt is
    // reserved for the actual parse failure; "[]" still parses to a
    // present-but-empty vector.
    EXPECT_FALSE(lvt::wpf_parse_tree_json("{not json", "wpf").has_value());

    auto emptyButValid = lvt::wpf_parse_tree_json("[]", "wpf");
    ASSERT_TRUE(emptyButValid.has_value());
    EXPECT_TRUE(emptyButValid->empty());
}

// ---- Watch diff ----

static Element diff_el(const std::string& type, const std::string& className,
                       const std::string& text = "") {
    Element el;
    el.type = type;
    el.framework = "win32";
    el.className = className;
    el.text = text;
    return el;
}

static Element diff_provider_el(const std::string& framework,
                                const std::string& type,
                                const std::string& className,
                                uint64_t providerHandle,
                                const std::string& text = "") {
    Element el;
    el.type = type;
    el.framework = framework;
    el.className = className;
    el.providerHandle = providerHandle;
    el.text = text;
    return el;
}

TEST(WatchDiff, AddedElement) {
    auto before = diff_el("Window", "Root");
    auto after = before;
    after.children.push_back(diff_el("Button", "Button", "OK"));

    auto events = diff_trees(before, after);

    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].type, ChangeEvent::Type::Added);
    EXPECT_EQ(events[0].path, "0.0");
    EXPECT_EQ(events[0].element.text, "OK");
}

TEST(WatchDiff, RemovedElement) {
    auto before = diff_el("Window", "Root");
    before.children.push_back(diff_el("Button", "Button", "OK"));
    auto after = diff_el("Window", "Root");

    auto events = diff_trees(before, after);

    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].type, ChangeEvent::Type::Removed);
    EXPECT_EQ(events[0].path, "0.0");
    EXPECT_EQ(events[0].element.text, "OK");
}

TEST(WatchDiff, ChangedElementFields) {
    auto before = diff_el("Window", "Root");
    before.children.push_back(diff_el("Button", "Button", "OK"));
    before.children[0].bounds = {1, 2, 3, 4};
    before.children[0].properties["enabled"] = "true";

    auto after = before;
    after.children[0].text = "Cancel";
    after.children[0].bounds = {5, 6, 7, 8};
    after.children[0].properties["enabled"] = "false";

    auto events = diff_trees(before, after);

    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].type, ChangeEvent::Type::Changed);
    EXPECT_EQ(events[0].fields["text"].oldValue, "OK");
    EXPECT_EQ(events[0].fields["text"].newValue, "Cancel");
    EXPECT_EQ(events[0].fields["bounds"].oldValue, "1,2,3,4");
    EXPECT_EQ(events[0].fields["bounds"].newValue, "5,6,7,8");
    EXPECT_EQ(events[0].fields["properties.enabled"].oldValue, "true");
    EXPECT_EQ(events[0].fields["properties.enabled"].newValue, "false");
}

TEST(WatchDiff, MovedElement) {
    auto before = diff_el("Window", "Root");
    before.children.push_back(diff_el("Pane", "Pane"));
    before.children.push_back(diff_el("Button", "Button", "OK"));

    auto after = diff_el("Window", "Root");
    after.children.push_back(diff_el("Pane", "Pane"));
    after.children[0].children.push_back(diff_el("Button", "Button", "OK"));

    auto events = diff_trees(before, after);

    // A structural Win32 element without a process-wide provider identity
    // still shows reparenting as Removed (old key) + Added (new key):
    // assign_element_keys (the one, single key algorithm this and dump/query/
    // UIA output all share — see its doc comment in element_key.h) threads
    // the full parent chain into this element's key, precisely because that
    // per-parent locality is what keeps an unrelated change elsewhere in the
    // tree from ever touching this element's key at all (see the
    // DuplicateIdentityElsewhereDoesNotDestabilizeUnrelatedSubtree test
    // right below). Reparenting changes that chain, so without a stronger
    // provider contract the information is correctly conveyed as two events.
    //
    // Checked by type+path rather than by vector index/order: diff_trees
    // emits all Added events before all Removed events (see its own
    // implementation), which is an implementation detail this test should
    // not need to know about.
    ASSERT_EQ(events.size(), 2);
    for (const auto& event : events)
        EXPECT_EQ(event.element.text, "OK");
    auto hasEvent = [&](ChangeEvent::Type type, const std::string& path) {
        return std::any_of(events.begin(), events.end(), [&](const ChangeEvent& e) {
            return e.type == type && e.path == path;
        });
    };
    EXPECT_TRUE(hasEvent(ChangeEvent::Type::Removed, "0.1"));
    EXPECT_TRUE(hasEvent(ChangeEvent::Type::Added, "0.0.0"));
}

TEST(WatchDiff, ProcessWideProviderHandleReparentsFromEarlierToLaterParent) {
    auto before = diff_el("Window", "Root");
    before.children.push_back(diff_el("Pane", "Earlier"));
    before.children.push_back(diff_el("Pane", "Later"));
    before.children[0].children.push_back(diff_provider_el(
        "xaml", "Button", "Windows.UI.Xaml.Controls.Button", 0xA01, "Move"));

    auto after = diff_el("Window", "Root");
    after.children.push_back(diff_el("Pane", "Earlier"));
    after.children.push_back(diff_el("Pane", "Later"));
    after.children[1].children.push_back(diff_provider_el(
        "xaml", "Button", "Windows.UI.Xaml.Controls.Button", 0xA01, "Move"));

    auto events = diff_trees(before, after);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, ChangeEvent::Type::Changed);
    EXPECT_EQ(events[0].key, "xaml:0xA01");
    EXPECT_EQ(events[0].path, "0.1.0");
    ASSERT_TRUE(events[0].fields.count("path"));
    EXPECT_EQ(events[0].fields["path"].oldValue, "0.0.0");
    EXPECT_EQ(events[0].fields["path"].newValue, "0.1.0");
    EXPECT_EQ(after.children[1].children[0].key,
              before.children[0].children[0].key);
}

TEST(WatchDiff, ProcessWideProviderHandleReparentsFromLaterToEarlierParent) {
    auto before = diff_el("Window", "Root");
    before.children.push_back(diff_el("Pane", "Earlier"));
    before.children.push_back(diff_el("Pane", "Later"));
    before.children[1].children.push_back(diff_provider_el(
        "winui3", "Button", "Microsoft.UI.Xaml.Controls.Button", 0xB01, "Move"));

    auto after = diff_el("Window", "Root");
    after.children.push_back(diff_el("Pane", "Earlier"));
    after.children.push_back(diff_el("Pane", "Later"));
    after.children[0].children.push_back(diff_provider_el(
        "winui3", "Button", "Microsoft.UI.Xaml.Controls.Button", 0xB01, "Move"));

    auto events = diff_trees(before, after);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, ChangeEvent::Type::Changed);
    EXPECT_EQ(events[0].key, "winui3:0xB01");
    EXPECT_EQ(events[0].path, "0.0.0");
    ASSERT_TRUE(events[0].fields.count("path"));
    EXPECT_EQ(events[0].fields["path"].oldValue, "0.1.0");
    EXPECT_EQ(events[0].fields["path"].newValue, "0.0.0");
}

TEST(WatchDiff, SamePathDifferentParentEmitsExplicitRelocation) {
    auto before = diff_el("Window", "Root");
    before.children.push_back(diff_el("Pane", "OldPanel"));
    before.children[0].children.push_back(diff_provider_el(
        "winui3", "Button", "Microsoft.UI.Xaml.Controls.Button", 0xB02, "Move"));

    auto after = diff_el("Window", "Root");
    after.children.push_back(diff_el("Pane", "NewPanel"));
    after.children[0].children.push_back(diff_provider_el(
        "winui3", "Button", "Microsoft.UI.Xaml.Controls.Button", 0xB02, "Move"));

    auto events = diff_trees(before, after);

    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].type, ChangeEvent::Type::Added);
    EXPECT_EQ(events[0].path, "0.0");
    EXPECT_EQ(events[1].type, ChangeEvent::Type::Changed);
    EXPECT_EQ(events[1].key, "winui3:0xB02");
    EXPECT_EQ(events[1].path, "0.0.0");
    EXPECT_FALSE(events[1].fields.count("path"));
    ASSERT_TRUE(events[1].fields.count("parentKey"));
    EXPECT_EQ(events[1].fields.at("parentKey").oldValue,
              "win32|Root/win32|OldPanel");
    EXPECT_EQ(events[1].fields.at("parentKey").newValue,
              "win32|Root/win32|NewPanel");
    EXPECT_EQ(events[2].type, ChangeEvent::Type::Removed);
    EXPECT_EQ(events[2].path, "0.0");

    auto serialized = json::parse(serialize_change_event(events[1]));
    EXPECT_EQ(serialized["path"], "0.0.0");
    EXPECT_EQ(serialized["fields"]["parentKey"]["old"],
              "win32|Root/win32|OldPanel");
    EXPECT_EQ(serialized["fields"]["parentKey"]["new"],
              "win32|Root/win32|NewPanel");
}

TEST(WatchDiff, ReparentedProviderSubtreeIsReconciledExactlyOnce) {
    auto makeSubtree = [] {
        auto root = diff_provider_el(
            "winui3", "Grid", "Microsoft.UI.Xaml.Controls.Grid", 0xC01);
        auto child = diff_provider_el(
            "winui3", "Border", "Microsoft.UI.Xaml.Controls.Border", 0xC02);
        child.children.push_back(diff_provider_el(
            "winui3", "TextBlock", "Microsoft.UI.Xaml.Controls.TextBlock",
            0xC03, "Stable"));
        root.children.push_back(std::move(child));
        return root;
    };

    auto before = diff_el("Window", "Root");
    before.children.push_back(diff_el("Pane", "Earlier"));
    before.children.push_back(diff_el("Pane", "Later"));
    before.children[0].children.push_back(makeSubtree());

    auto after = diff_el("Window", "Root");
    after.children.push_back(diff_el("Pane", "Earlier"));
    after.children.push_back(diff_el("Pane", "Later"));
    after.children[1].children.push_back(makeSubtree());
    after.children[1].children[0].children[0].children[0].text = "Updated";

    auto events = diff_trees(before, after);

    ASSERT_EQ(events.size(), 3u);
    EXPECT_TRUE(std::none_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == ChangeEvent::Type::Added ||
               event.type == ChangeEvent::Type::Removed;
    }));

    const std::map<std::string, std::pair<std::string, std::string>> expectedPaths{
        {"winui3:0xC01", {"0.0.0", "0.1.0"}},
        {"winui3:0xC02", {"0.0.0.0", "0.1.0.0"}},
        {"winui3:0xC03", {"0.0.0.0.0", "0.1.0.0.0"}},
    };
    for (const auto& [key, paths] : expectedPaths) {
        auto matches = std::count_if(events.begin(), events.end(), [&](const auto& event) {
            return event.key == key;
        });
        EXPECT_EQ(matches, 1) << key << " was reconciled more than once";
        auto event = std::find_if(events.begin(), events.end(), [&](const auto& candidate) {
            return candidate.key == key;
        });
        ASSERT_NE(event, events.end());
        ASSERT_TRUE(event->fields.count("path"));
        EXPECT_EQ(event->fields.at("path").oldValue, paths.first);
        EXPECT_EQ(event->fields.at("path").newValue, paths.second);
    }
    auto leaf = std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.key == "winui3:0xC03";
    });
    ASSERT_NE(leaf, events.end());
    ASSERT_TRUE(leaf->fields.count("text"));
    EXPECT_EQ(leaf->fields.at("text").oldValue, "Stable");
    EXPECT_EQ(leaf->fields.at("text").newValue, "Updated");
}

TEST(WatchDiff, RecycledIncompatibleProviderHandleIsNotGloballyMatched) {
    auto before = diff_el("Window", "Root");
    before.children.push_back(diff_el("Pane", "Earlier"));
    before.children.push_back(diff_el("Pane", "Later"));
    before.children[0].children.push_back(diff_provider_el(
        "xaml", "Button", "Windows.UI.Xaml.Controls.Button", 0xD01, "Old"));

    auto after = diff_el("Window", "Root");
    after.children.push_back(diff_el("Pane", "Earlier"));
    after.children.push_back(diff_el("Pane", "Later"));
    after.children[1].children.push_back(diff_provider_el(
        "xaml", "TextBlock", "Windows.UI.Xaml.Controls.TextBlock", 0xD01, "New"));

    auto events = diff_trees(before, after);

    ASSERT_EQ(events.size(), 2u);
    EXPECT_TRUE(std::none_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == ChangeEvent::Type::Changed;
    }));
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) {
        return event.type == ChangeEvent::Type::Added;
    }), 1);
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) {
        return event.type == ChangeEvent::Type::Removed;
    }), 1);
}

TEST(WatchDiff, DuplicateProviderHandlesAreNotGloballyMatched) {
    auto before = diff_el("Window", "Root");
    before.children.push_back(diff_el("Pane", "Earlier"));
    before.children.push_back(diff_el("Pane", "Later"));
    before.children[0].children.push_back(diff_provider_el(
        "winui3", "Item", "Microsoft.UI.Xaml.Controls.Item", 0xE01, "One"));
    before.children[0].children.push_back(diff_provider_el(
        "winui3", "Item", "Microsoft.UI.Xaml.Controls.Item", 0xE01, "Two"));

    auto after = diff_el("Window", "Root");
    after.children.push_back(diff_el("Pane", "Earlier"));
    after.children.push_back(diff_el("Pane", "Later"));
    after.children[1].children.push_back(diff_provider_el(
        "winui3", "Item", "Microsoft.UI.Xaml.Controls.Item", 0xE01, "One"));
    after.children[1].children.push_back(diff_provider_el(
        "winui3", "Item", "Microsoft.UI.Xaml.Controls.Item", 0xE01, "Two"));

    auto events = diff_trees(before, after);

    ASSERT_EQ(events.size(), 4u);
    EXPECT_TRUE(std::none_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == ChangeEvent::Type::Changed;
    }));
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) {
        return event.type == ChangeEvent::Type::Added;
    }), 2);
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) {
        return event.type == ChangeEvent::Type::Removed;
    }), 2);
}

TEST(WatchDiff, ProviderHandleWithoutProcessWideContractIsNotGloballyMatched) {
    auto before = diff_el("Window", "Root");
    before.children.push_back(diff_el("Pane", "Earlier"));
    before.children.push_back(diff_el("Pane", "Later"));
    before.children[0].children.push_back(diff_provider_el(
        "wpf", "Button", "System.Windows.Controls.Button", 0xF01, "Move"));

    auto after = diff_el("Window", "Root");
    after.children.push_back(diff_el("Pane", "Earlier"));
    after.children.push_back(diff_el("Pane", "Later"));
    after.children[1].children.push_back(diff_provider_el(
        "wpf", "Button", "System.Windows.Controls.Button", 0xF01, "Move"));

    auto events = diff_trees(before, after);

    ASSERT_EQ(events.size(), 2u);
    EXPECT_TRUE(std::none_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == ChangeEvent::Type::Changed;
    }));
}

TEST(WatchDiff, DuplicateIdentityElsewhereDoesNotDestabilizeUnrelatedSubtree) {
    // Regression test for the actual reported bug ("the tree rebuilds while
    // navigating", reproduced live against Microsoft Store's tree): watch's
    // diffing used to compute its key discriminator from a GLOBAL,
    // whole-tree count of each identity (framework/className) plus the full
    // root-to-node path as the fallback — meaning a change ANYWHERE that
    // shared an identity with an element elsewhere in the tree could change
    // that unrelated element's key too. A real XAML tree is thick with
    // duplicate-identity elements (Grid/Border/TextBlock/ContentPresenter/
    // Rectangle/...), so this showed up live as large, unrelated portions
    // of the tree looking removed-and-re-added on every tick.
    // assign_element_keys' per-parent scoping (the same algorithm dump/
    // query/UIA output already used) means only an element's OWN siblings
    // can ever affect its key — nothing outside its own parent can.
    auto before = diff_el("Window", "Root");
    before.children.push_back(diff_el("Pane", "GroupA"));
    before.children[0].children.push_back(diff_el("Text", "TextBlock", "A1"));
    before.children.push_back(diff_el("Pane", "GroupB"));
    before.children[1].children.push_back(diff_el("Text", "TextBlock", "B1"));

    // GroupB (entirely unrelated to GroupA) gains a same-identity sibling —
    // the exact shape of change that used to cascade globally.
    auto after = before;
    after.children[1].children.insert(after.children[1].children.begin(),
                                      diff_el("Text", "TextBlock", "B0"));

    auto events = diff_trees(before, after);

    // Whatever this does to GroupB's own children (a real, bounded, locally
    // -scoped side effect of inserting a sibling ahead of an existing one —
    // see WatchDiff.MovedElement for the same trade-off applied to
    // reparenting) must never touch GroupA's "A1", which nothing here
    // changed at all.
    for (const auto& event : events)
        EXPECT_NE(event.element.text, "A1")
            << "an unrelated GroupB change destabilized GroupA's element";
}

TEST(WatchDiff, AncestorSiblingChurnDoesNotDestabilizeStableDescendants) {
    // Regression test for the deeper version of the reported bug ("the
    // tree rebuilds while navigating"): the fix above (per-parent-scoped
    // disambiguation) stops an unrelated *subtree* from destabilizing
    // another, but a purely structural key — recomputed fresh from a
    // tree's shape every tick, with no memory of previous ticks — still
    // threads every ancestor's own disambiguating segment into each of its
    // descendants' keys. So if an ancestor's *own* position among its own
    // same-identity siblings shifts (extremely common in a live, animated
    // UI: a virtualized/recycled list item, a carousel auto-rotating),
    // every element beneath it gets a brand-new key on that tick even
    // though nothing about it individually changed. Verified live: a
    // passively watched, completely untouched Microsoft Store home page
    // (whose carousel auto-rotates) showed nearly its *entire* tree
    // repeatedly flip-flopping between removed and re-added, purely from
    // time passing, not from anything a user did.
    //
    // Cross-tick reconciliation (diff_trees now matches each tick's tree
    // against the previous one and lets a matched node INHERIT its
    // predecessor's key, rather than ever recomputing it from scratch) is
    // what actually fixes this: a stable descendant survives even when its
    // own ancestor's local position moves, as long as reconciliation still
    // recognizes that ancestor as "the same slot, just moved" — which the
    // one-level-deep shape fingerprint (see reconcile_children) is what
    // lets it do here, since the tracked card is distinguishable from the
    // newly inserted, differently-shaped one.
    auto before = diff_el("Window", "Root");
    before.children.push_back(diff_el("Card", "Card"));          // slot 0
    before.children.push_back(diff_el("Card", "Card"));          // slot 1 -- tracked
    before.children[1].children.push_back(diff_el("Text", "TextBlock", "Stable"));

    // A new, differently-shaped same-identity sibling is inserted *before*
    // the tracked card, shifting its own local index from 1 to 2 -- the
    // exact shape of change a recycled/virtualized list item produces.
    auto after = diff_el("Window", "Root");
    after.children.push_back(diff_el("Card", "Card"));
    after.children.push_back(diff_el("Card", "Card"));            // newly inserted, empty
    after.children.push_back(diff_el("Card", "Card"));            // the tracked card, now at index 2
    after.children[2].children.push_back(diff_el("Text", "TextBlock", "Stable"));

    auto events = diff_trees(before, after);

    // The tracked card's own child must never show up as Added or Removed
    // (its parent moving is expected to still surface as a legitimate
    // Changed/path event for the child too, since the child's own reported
    // position shifted from "0.1.0" to "0.2.0" — that is correct,
    // informative structural news, not the bug); it must never be treated
    // as a brand-new/disappeared element.
    for (const auto& event : events) {
        if (event.element.text != "Stable")
            continue;
        EXPECT_NE(event.type, ChangeEvent::Type::Added)
            << "an ancestor's own index shift destabilized a stable descendant (reported as Added)";
        EXPECT_NE(event.type, ChangeEvent::Type::Removed)
            << "an ancestor's own index shift destabilized a stable descendant (reported as Removed)";
    }

    bool sawTrackedCardMove = std::any_of(events.begin(), events.end(), [](const ChangeEvent& e) {
        return e.type == ChangeEvent::Type::Changed &&
               e.fields.count("path") &&
               e.fields.at("path").oldValue == "0.1" && e.fields.at("path").newValue == "0.2";
    });
    EXPECT_TRUE(sawTrackedCardMove) << "expected the tracked card to be recognized as moved, not replaced";
}

TEST(WatchDiff, ProviderHandlesAbove32BitsSurviveSiblingReordering) {
    Element before = diff_el("Window", "Root");
    Element first = diff_el("Item", "Item", "First");
    first.providerHandle = UINT64_C(0x100000001);
    Element second = diff_el("Item", "Item", "Second");
    second.providerHandle = UINT64_C(0x200000001);
    before.children = {first, second};

    Element after = diff_el("Window", "Root");
    after.children = {second, first};

    auto events = diff_trees(before, after);

    EXPECT_TRUE(std::none_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == ChangeEvent::Type::Added ||
               event.type == ChangeEvent::Type::Removed;
    }));
    EXPECT_NE(after.children[0].key.find("200000001"), std::string::npos);
    EXPECT_NE(after.children[1].key.find("100000001"), std::string::npos);
}

TEST(BoundedEventQueue, OverflowRequiresSnapshotAndRecoversAfterDrain) {
    BoundedEventQueue<int, 2> queue;
    queue.push(1);
    queue.push(2);
    queue.push(3);

    auto overflow = queue.drain();
    EXPECT_TRUE(overflow.snapshotRequired);
    EXPECT_TRUE(overflow.events.empty());

    queue.push(4);
    auto recovered = queue.drain();
    EXPECT_FALSE(recovered.snapshotRequired);
    ASSERT_EQ(recovered.events.size(), 1u);
    EXPECT_EQ(recovered.events[0], 4);
}

TEST(NativeWinEventQueue, BurstOverflowIsBoundedAndRecoversAfterDrain) {
    using namespace lvt::native_eventing_detail;
    NativeWinEventQueue<4> queue;
    const NativeWinEventRecord event{
        EVENT_OBJECT_REORDER,
        reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234)),
        OBJID_CLIENT,
        CHILDID_SELF};

    EXPECT_TRUE(queue.push(event));
    EXPECT_TRUE(queue.push(event));
    EXPECT_TRUE(queue.push(event));
    EXPECT_TRUE(queue.push(event));
    EXPECT_FALSE(queue.push(event));

    size_t drainedCount = 0;
    EXPECT_TRUE(queue.drain(
        [&](const NativeWinEventRecord&) noexcept {
            ++drainedCount;
        }));
    EXPECT_EQ(drainedCount, 4u);

    drainedCount = 0;
    EXPECT_TRUE(queue.push(event));
    EXPECT_FALSE(queue.drain(
        [&](const NativeWinEventRecord&) noexcept {
            ++drainedCount;
        }));
    EXPECT_EQ(drainedCount, 1u);
}

TEST(NativeWinEventRootLifecycle, OnlyTheRootWindowObjectMarksTheRootDestroyed) {
    using namespace lvt::native_eventing_detail;
    const HWND root =
        reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));
    const HWND other =
        reinterpret_cast<HWND>(static_cast<uintptr_t>(0x5678));

    EXPECT_FALSE(event_destroys_root_window(
        root,
        {EVENT_OBJECT_DESTROY, root, OBJID_CLIENT, 1},
        true));
    EXPECT_FALSE(event_destroys_root_window(
        root,
        {EVENT_OBJECT_DESTROY, root, OBJID_WINDOW, 1},
        false));
    EXPECT_FALSE(event_destroys_root_window(
        root,
        {EVENT_OBJECT_DESTROY, root, OBJID_CLIENT, CHILDID_SELF},
        true));
    EXPECT_TRUE(event_destroys_root_window(
        root,
        {EVENT_OBJECT_DESTROY, root, OBJID_WINDOW, CHILDID_SELF},
        true));
    EXPECT_TRUE(event_destroys_root_window(
        root,
        {EVENT_OBJECT_DESTROY, root, OBJID_CLIENT, CHILDID_SELF},
        false));
    EXPECT_FALSE(event_destroys_root_window(
        root,
        {EVENT_OBJECT_DESTROY, other, OBJID_WINDOW, CHILDID_SELF},
        false));
}

TEST(OverlappedIo, CancellationCompletesBeforeStackStorageIsReleased) {
    const auto pipeName =
        std::wstring(L"\\\\.\\pipe\\lvt_overlapped_test_") +
        std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64());
    wil::unique_hfile server(CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 64, 64, 0, nullptr));
    ASSERT_TRUE(server);

    wil::unique_event connectEvent(
        CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(connectEvent);
    OVERLAPPED connectOv{};
    connectOv.hEvent = connectEvent.get();
    ASSERT_FALSE(ConnectNamedPipe(server.get(), &connectOv));
    ASSERT_EQ(GetLastError(), ERROR_IO_PENDING);

    wil::unique_hfile client(CreateFileW(
        pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(client);
    ASSERT_EQ(WaitForSingleObject(connectEvent.get(), 5000), WAIT_OBJECT_0);
    DWORD connectedBytes = 0;
    ASSERT_TRUE(GetOverlappedResult(
        server.get(), &connectOv, &connectedBytes, FALSE));

    char buffer[8]{};
    wil::unique_event readEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(readEvent);
    OVERLAPPED readOv{};
    readOv.hEvent = readEvent.get();
    DWORD bytesRead = 0;
    ASSERT_FALSE(ReadFile(
        server.get(), buffer, sizeof(buffer), &bytesRead, &readOv));
    ASSERT_EQ(GetLastError(), ERROR_IO_PENDING);

    detail::cancel_and_complete_overlapped(server.get(), readOv);

    SetLastError(ERROR_SUCCESS);
    EXPECT_FALSE(GetOverlappedResult(
        server.get(), &readOv, &bytesRead, FALSE));
    EXPECT_EQ(GetLastError(), ERROR_OPERATION_ABORTED);
}

TEST(OverlappedIo, PendingConnectCancellationIsDrained) {
    const auto pipeName =
        std::wstring(L"\\\\.\\pipe\\lvt_connect_cancel_") +
        std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64());
    wil::unique_hfile pipe(CreateNamedPipeW(
        pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 64, 64, 0, nullptr));
    ASSERT_TRUE(pipe);
    wil::unique_event event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    ASSERT_FALSE(ConnectNamedPipe(pipe.get(), &overlapped));
    ASSERT_EQ(GetLastError(), ERROR_IO_PENDING);

    detail::cancel_and_complete_overlapped(pipe.get(), overlapped);

    DWORD transferred = 0;
    EXPECT_FALSE(GetOverlappedResult(
        pipe.get(), &overlapped, &transferred, FALSE));
    EXPECT_EQ(GetLastError(), ERROR_OPERATION_ABORTED);
}

TEST(OverlappedIo, PendingWriteCancellationIsDrained) {
    const auto pipeName =
        std::wstring(L"\\\\.\\pipe\\lvt_write_cancel_") +
        std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64());
    wil::unique_hfile server(CreateNamedPipeW(
        pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 1, 1, 0, nullptr));
    ASSERT_TRUE(server);
    wil::unique_event connectEvent(
        CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED connectOv{};
    connectOv.hEvent = connectEvent.get();
    ASSERT_FALSE(ConnectNamedPipe(server.get(), &connectOv));
    ASSERT_EQ(GetLastError(), ERROR_IO_PENDING);
    wil::unique_hfile client(CreateFileW(
        pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(client);
    ASSERT_EQ(WaitForSingleObject(connectEvent.get(), 5000), WAIT_OBJECT_0);
    DWORD connected = 0;
    ASSERT_TRUE(GetOverlappedResult(
        server.get(), &connectOv, &connected, FALSE));

    std::vector<char> payload(1024 * 1024, 'x');
    wil::unique_event writeEvent(
        CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED writeOv{};
    writeOv.hEvent = writeEvent.get();
    DWORD written = 0;
    ASSERT_FALSE(WriteFile(
        server.get(), payload.data(), static_cast<DWORD>(payload.size()),
        &written, &writeOv));
    ASSERT_EQ(GetLastError(), ERROR_IO_PENDING);

    detail::cancel_and_complete_overlapped(server.get(), writeOv);

    EXPECT_FALSE(GetOverlappedResult(
        server.get(), &writeOv, &written, FALSE));
    EXPECT_EQ(GetLastError(), ERROR_OPERATION_ABORTED);
}

TEST(WatchDiff, SerializeChangedEvent) {
    ChangeEvent event;
    event.type = ChangeEvent::Type::Changed;
    event.key = "win32|Button|Button";
    event.path = "0.1.0";
    event.fields["text"] = {"OK", "Cancel"};
    event.fields["path"] = {"0.0", "0.1.0"};

    auto j = json::parse(serialize_change_event(event));
    EXPECT_EQ(j["event"], "changed");
    EXPECT_EQ(j["path"], "0.1.0");
    EXPECT_EQ(j["fields"]["text"]["old"], "OK");
    EXPECT_EQ(j["fields"]["text"]["new"], "Cancel");
    EXPECT_EQ(j["fields"]["path"]["old"], "0.0");
    EXPECT_EQ(j["fields"]["path"]["new"], "0.1.0");
}

// ---- Large tree serialization ----

TEST(JsonSerializer, LargeTree) {
    Element root;
    root.type = "Root";
    root.framework = "win32";
    for (int i = 0; i < 100; i++) {
        Element child;
        child.type = "Item" + std::to_string(i);
        child.framework = "win32";
        child.text = "text" + std::to_string(i);
        root.children.push_back(child);
    }
    assign_element_ids(root);
    auto result = serialize_to_json(root, nullptr, 0, "test.exe", {"win32"});
    auto j = json::parse(result);
    EXPECT_EQ(j["root"]["children"].size(), 100);
    EXPECT_EQ(j["root"]["children"][99]["id"], "e100");
}

TEST(XmlSerializer, LargeTree) {
    Element root;
    root.type = "Root";
    root.framework = "win32";
    for (int i = 0; i < 100; i++) {
        Element child;
        child.type = "Item";
        child.framework = "win32";
        root.children.push_back(child);
    }
    assign_element_ids(root);
    auto result = serialize_to_xml(root, nullptr, 0, "test.exe", {"win32"});
    // Count occurrences of "<Item"
    size_t count = 0;
    size_t pos = 0;
    while ((pos = result.find("<Item", pos)) != std::string::npos) {
        count++;
        pos++;
    }
    EXPECT_EQ(count, 100);
}

TEST(XmlSerializer, MultipleFrameworksList) {
    auto root = make_test_tree();
    auto result = serialize_to_xml(root, nullptr, 0, "test.exe", {"win32", "comctl"});
    EXPECT_NE(result.find("frameworks=\"win32,comctl\""), std::string::npos);
}

// ---- Architecture detection ----

TEST(Architecture, NameStrings) {
    EXPECT_STREQ(architecture_name(Architecture::x64), "x64");
    EXPECT_STREQ(architecture_name(Architecture::arm64), "arm64");
    EXPECT_STREQ(architecture_name(Architecture::unknown), "unknown");
}

TEST(Architecture, HostArchitecture) {
    auto host = get_host_architecture();
#if defined(_M_ARM64)
    EXPECT_EQ(host, Architecture::arm64);
#elif defined(_M_X64)
    EXPECT_EQ(host, Architecture::x64);
#endif
}

TEST(Architecture, DetectCurrentProcess) {
    auto arch = detect_process_architecture(GetCurrentProcessId());
    // Current process must match host
    EXPECT_EQ(arch, get_host_architecture());
}

TEST(Architecture, DetectInvalidPid) {
    // PID 0 (System Idle Process) — OpenProcess will fail
    auto arch = detect_process_architecture(0);
    // Should fall back to host architecture
    EXPECT_EQ(arch, get_host_architecture());
}

// ---- Plugin grafting tests ----

#include "plugin_loader.h"

// Mock plugin: returns canned JSON from enrich()
static const char* s_mockJson = nullptr;

static LvtPluginInfo s_mockInfo = {
    sizeof(LvtPluginInfo), LVT_PLUGIN_API_VERSION, "mock", "Mock plugin"
};
static LvtPluginInfo* mock_plugin_info() { return &s_mockInfo; }
static int mock_detect(DWORD, HWND, LvtFrameworkDetection*) { return 0; }
static int mock_enrich(HWND, DWORD, const char*, char** json_out) {
    if (!s_mockJson) return 0;
    *json_out = _strdup(s_mockJson);
    return 1;
}
static void mock_free(void* p) { free(p); }

static LoadedPlugin make_mock_plugin() {
    LoadedPlugin lp{};
    lp.info = &s_mockInfo;
    lp.detect = mock_detect;
    lp.enrich = mock_enrich;
    lp.free_fn = mock_free;
    return lp;
}

static PluginFrameworkInfo make_mock_fw(const LoadedPlugin* p) {
    return {"mock", "", p};
}

namespace {

struct PersistentPluginMockState {
    enum class EventMode {
        normal,
        oversized,
        nullPointer,
        shortStruct,
        unknownMutation,
    };

    int opens = 0;
    int gets = 0;
    int polls = 0;
    int frees = 0;
    int eventFrees = 0;
    int closes = 0;
    int failGetAt = 0;
    std::string treeJson = "[]";
    std::string filter;
    EventMode eventMode = EventMode::normal;
};

PersistentPluginMockState s_persistentPlugin;

void* persistent_mock_open(HWND, DWORD) {
    ++s_persistentPlugin.opens;
    return &s_persistentPlugin;
}

int persistent_mock_get_tree(void*, const char* filter, char** jsonOut) {
    const int call = ++s_persistentPlugin.gets;
    s_persistentPlugin.filter = filter ? filter : "";
    if (call == s_persistentPlugin.failGetAt)
        return 0;
    *jsonOut = _strdup(s_persistentPlugin.treeJson.c_str());
    return *jsonOut != nullptr;
}

int persistent_mock_poll(
    void*, LvtConnectionEvent** eventsOut, uint32_t* countOut) {
    ++s_persistentPlugin.polls;
    if (s_persistentPlugin.eventMode ==
        PersistentPluginMockState::EventMode::nullPointer) {
        *eventsOut = nullptr;
        *countOut = 1;
        return 1;
    }
    auto* events = static_cast<LvtConnectionEvent*>(
        malloc(sizeof(LvtConnectionEvent)));
    if (!events)
        return 0;
    *events = {
        s_persistentPlugin.eventMode ==
                PersistentPluginMockState::EventMode::shortStruct
            ? sizeof(uint32_t)
            : sizeof(LvtConnectionEvent),
        s_persistentPlugin.eventMode ==
                PersistentPluginMockState::EventMode::unknownMutation
            ? "replace"
            : "remove",
        42, 7, 3, nullptr, nullptr};
    *eventsOut = events;
    *countOut =
        s_persistentPlugin.eventMode ==
                PersistentPluginMockState::EventMode::oversized
            ? 10001
            : 1;
    return 1;
}

void persistent_mock_free(void* pointer) {
    ++s_persistentPlugin.frees;
    free(pointer);
}

void persistent_mock_events_free(
    LvtConnectionEvent* events, uint32_t) {
    ++s_persistentPlugin.eventFrees;
    free(events);
}

void persistent_mock_close(void*) {
    ++s_persistentPlugin.closes;
}

LoadedPlugin make_persistent_mock_plugin() {
    LoadedPlugin plugin{};
    plugin.info = &s_mockInfo;
    plugin.enrich = mock_enrich;
    plugin.free_fn = persistent_mock_free;
    plugin.connection_open = persistent_mock_open;
    plugin.connection_get_tree = persistent_mock_get_tree;
    plugin.connection_poll_events = persistent_mock_poll;
    plugin.connection_events_free = persistent_mock_events_free;
    plugin.connection_close = persistent_mock_close;
    return plugin;
}

} // namespace

TEST(PluginConnectionContract, RequiresCompleteLifetimeAndPollingGroups) {
    auto plugin = make_persistent_mock_plugin();
    EXPECT_TRUE(plugin_supports_persistent_connections(plugin));
    EXPECT_TRUE(plugin_supports_event_polling(plugin));

    plugin.connection_close = nullptr;
    EXPECT_FALSE(plugin_supports_persistent_connections(plugin));
    plugin.connection_close = persistent_mock_close;
    plugin.free_fn = nullptr;
    EXPECT_FALSE(plugin_supports_persistent_connections(plugin));

    plugin = make_persistent_mock_plugin();
    plugin.connection_events_free = nullptr;
    EXPECT_FALSE(plugin_supports_event_polling(plugin));
    EXPECT_TRUE(plugin_supports_persistent_connections(plugin));
}

TEST(PluginConnectionContract, AdapterReusesTreeOptionPollsAndClosesOnce) {
    s_persistentPlugin = {};
    auto plugin = make_persistent_mock_plugin();
    auto connection =
        open_plugin_connection(make_mock_fw(&plugin), nullptr, 123);
    ASSERT_NE(connection, nullptr);
    EXPECT_EQ(s_persistentPlugin.opens, 1);

    Element root;
    EXPECT_TRUE(connection->get_tree(root, false, "title:Dashboard"));
    EXPECT_TRUE(connection->get_tree(root, true, "url:*example*"));
    EXPECT_EQ(s_persistentPlugin.gets, 2);
    EXPECT_EQ(s_persistentPlugin.frees, 2);
    EXPECT_EQ(s_persistentPlugin.filter, "url:*example*");

    const auto events = connection->poll_events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].mutation, ConnectionEvent::Mutation::removed);
    EXPECT_EQ(events[0].handle, 42u);
    EXPECT_EQ(events[0].parentHandle, 7u);
    EXPECT_EQ(events[0].childIndex, 3);
    EXPECT_EQ(s_persistentPlugin.polls, 1);
    EXPECT_EQ(s_persistentPlugin.eventFrees, 1);

    connection.reset();
    EXPECT_EQ(s_persistentPlugin.closes, 1);
}

TEST(PluginConnectionContract, FailedRefreshMarksConnectionDead) {
    s_persistentPlugin = {};
    s_persistentPlugin.failGetAt = 1;
    auto plugin = make_persistent_mock_plugin();
    auto connection =
        open_plugin_connection(make_mock_fw(&plugin), nullptr, 123);
    ASSERT_NE(connection, nullptr);

    Element root;
    EXPECT_FALSE(connection->get_tree(root, false));
    EXPECT_FALSE(connection->is_alive());
    connection.reset();
    EXPECT_EQ(s_persistentPlugin.closes, 1);
}

TEST(PluginConnectionContract, MalformedTreesAreFreedAndMarkConnectionDead) {
    for (const char* malformed : {
             "42",
             R"([{"children":{}}])",
             R"([{"offsetX":"left"}])",
             "{not-json",
         }) {
        SCOPED_TRACE(malformed);
        s_persistentPlugin = {};
        s_persistentPlugin.treeJson = malformed;
        auto plugin = make_persistent_mock_plugin();
        auto connection =
            open_plugin_connection(make_mock_fw(&plugin), nullptr, 123);
        ASSERT_NE(connection, nullptr);

        Element root;
        EXPECT_FALSE(connection->get_tree(root, false));
        EXPECT_FALSE(connection->is_alive());
        EXPECT_TRUE(root.children.empty());
        EXPECT_EQ(s_persistentPlugin.frees, 1);
        connection.reset();
        EXPECT_EQ(s_persistentPlugin.closes, 1);
    }
}

TEST(PluginConnectionContract, OversizedTreeIsRejectedBeforeGrafting) {
    s_persistentPlugin = {};
    s_persistentPlugin.treeJson.reserve(300005);
    s_persistentPlugin.treeJson = "[";
    for (int i = 0; i < 100001; ++i) {
        if (i > 0)
            s_persistentPlugin.treeJson += ",";
        s_persistentPlugin.treeJson += "{}";
    }
    s_persistentPlugin.treeJson += "]";

    auto plugin = make_persistent_mock_plugin();
    auto connection =
        open_plugin_connection(make_mock_fw(&plugin), nullptr, 123);
    ASSERT_NE(connection, nullptr);
    Element root;
    EXPECT_FALSE(connection->get_tree(root, false));
    EXPECT_FALSE(connection->is_alive());
    EXPECT_TRUE(root.children.empty());
    EXPECT_EQ(s_persistentPlugin.frees, 1);
    connection.reset();
    EXPECT_EQ(s_persistentPlugin.closes, 1);
}

TEST(PluginConnectionContract, MalformedEventsAreBoundedFreedAndMarkDead) {
    for (const auto mode : {
             PersistentPluginMockState::EventMode::oversized,
             PersistentPluginMockState::EventMode::nullPointer,
             PersistentPluginMockState::EventMode::shortStruct,
             PersistentPluginMockState::EventMode::unknownMutation,
         }) {
        s_persistentPlugin = {};
        s_persistentPlugin.eventMode = mode;
        auto plugin = make_persistent_mock_plugin();
        auto connection =
            open_plugin_connection(make_mock_fw(&plugin), nullptr, 123);
        ASSERT_NE(connection, nullptr);

        EXPECT_TRUE(connection->poll_events().empty());
        EXPECT_FALSE(connection->is_alive());
        EXPECT_EQ(
            s_persistentPlugin.eventFrees,
            mode == PersistentPluginMockState::EventMode::nullPointer ? 0 : 1);
        connection.reset();
        EXPECT_EQ(s_persistentPlugin.closes, 1);
    }
}

TEST(PluginConnectionContract, PersistentPluginWithoutPollStillRefreshes) {
    s_persistentPlugin = {};
    auto plugin = make_persistent_mock_plugin();
    plugin.connection_poll_events = nullptr;
    plugin.connection_events_free = nullptr;
    ASSERT_TRUE(plugin_supports_persistent_connections(plugin));
    ASSERT_FALSE(plugin_supports_event_polling(plugin));

    auto connection =
        open_plugin_connection(make_mock_fw(&plugin), nullptr, 123);
    ASSERT_NE(connection, nullptr);
    Element root;
    EXPECT_TRUE(connection->get_tree(root, false));
    EXPECT_TRUE(connection->get_tree(root, false));
    EXPECT_TRUE(connection->poll_events().empty());
    EXPECT_EQ(s_persistentPlugin.gets, 2);
    EXPECT_EQ(s_persistentPlugin.polls, 0);
    EXPECT_EQ(s_persistentPlugin.frees, 2);
    connection.reset();
    EXPECT_EQ(s_persistentPlugin.closes, 1);
}

TEST(PluginConnectionContract, V1AndPartialV2StayOneShot) {
    auto v1 = make_mock_plugin();
    EXPECT_FALSE(plugin_supports_persistent_connections(v1));
    EXPECT_EQ(open_plugin_connection(make_mock_fw(&v1), nullptr, 123), nullptr);

    auto partial = make_persistent_mock_plugin();
    partial.connection_close = nullptr;
    EXPECT_FALSE(plugin_supports_persistent_connections(partial));
    EXPECT_EQ(
        open_plugin_connection(make_mock_fw(&partial), nullptr, 123), nullptr);
}

TEST(PluginConnectionContract, RegistryLabelsIsolatePluginAndWindowInstances) {
    auto first = make_persistent_mock_plugin();
    auto second = make_persistent_mock_plugin();
    first.sourcePath = L"C:\\plugins\\first.dll";
    second.sourcePath = L"C:\\plugins\\second.dll";
    const PluginFrameworkInfo firstFramework{"mock", "", &first};
    const PluginFrameworkInfo secondFramework{"mock", "", &second};
    const auto firstLabel = plugin_connection_label(
        firstFramework, reinterpret_cast<HWND>(0x101));
    const auto otherPluginLabel = plugin_connection_label(
        secondFramework, reinterpret_cast<HWND>(0x101));
    const auto otherWindowLabel = plugin_connection_label(
        firstFramework, reinterpret_cast<HWND>(0x202));

    EXPECT_EQ(firstLabel.rfind("plugin:", 0), 0u);
    EXPECT_NE(firstLabel, "xaml");
    EXPECT_NE(firstLabel, otherPluginLabel);
    EXPECT_NE(firstLabel, otherWindowLabel);
}

TEST(FrameworkRefreshCompleteness, CopiesEveryProviderMarkerToScopedTree) {
    Element full;
    full.properties["lvt.incomplete.wpf"] = "true";
    full.properties["lvt.incomplete.fake-plugin"] = "true";
    full.properties["unrelated"] = "value";
    Element scoped;

    copy_incomplete_framework_refresh_markers(full, scoped);

    EXPECT_TRUE(framework_refresh_incomplete(scoped, "wpf"));
    EXPECT_TRUE(framework_refresh_incomplete(scoped, "fake-plugin"));
    EXPECT_FALSE(scoped.properties.contains("unrelated"));
}

TEST(PluginGraft, GraftByTargetHwnd) {
    // Build a simple Win32 tree: root -> child (hwnd=0x1234)
    Element root;
    root.type = "Window";
    root.framework = "win32";
    root.properties["hwnd"] = "0x1234";

    Element child;
    child.type = "Window";
    child.framework = "win32";
    child.className = "HostClass";
    child.properties["hwnd"] = "0xABCD";
    child.bounds = {100, 200, 300, 400};
    root.children.push_back(child);

    // Plugin returns JSON targeting hwnd 0xABCD
    s_mockJson = R"([{"target_hwnd":"0xABCD","type":"HostClass","children":[
        {"type":"PluginButton","name":"OK","width":80,"height":30,"offsetX":10,"offsetY":20}
    ]}])";

    auto lp = make_mock_plugin();
    auto fw = make_mock_fw(&lp);
    bool ok = enrich_with_plugin(root, nullptr, 0, fw);
    EXPECT_TRUE(ok);

    // The child at hwnd 0xABCD should now have a plugin child
    auto& host = root.children[0];
    ASSERT_EQ(host.children.size(), 1);
    EXPECT_EQ(host.children[0].type, "PluginButton");
    EXPECT_EQ(host.children[0].framework, "mock");
    EXPECT_EQ(host.children[0].text, "OK");
    EXPECT_EQ(host.children[0].bounds.width, 80);
    EXPECT_EQ(host.children[0].bounds.height, 30);
    // Coordinates are host base + offset
    EXPECT_EQ(host.children[0].bounds.x, 110);  // 100 + 10
    EXPECT_EQ(host.children[0].bounds.y, 220);  // 200 + 20
}

TEST(PluginGraft, GraftUnderRootWhenNoMatch) {
    Element root;
    root.type = "Window";
    root.framework = "win32";
    root.bounds = {0, 0, 800, 600};

    // Plugin returns JSON with a target_hwnd that doesn't exist
    s_mockJson = R"([{"target_hwnd":"0xDEAD","type":"Orphan","children":[
        {"type":"Child"}
    ]}])";

    auto lp = make_mock_plugin();
    auto fw = make_mock_fw(&lp);
    bool ok = enrich_with_plugin(root, nullptr, 0, fw);
    EXPECT_TRUE(ok);

    // Should graft under root since no match
    ASSERT_EQ(root.children.size(), 1);
    EXPECT_EQ(root.children[0].type, "Orphan");
    EXPECT_EQ(root.children[0].framework, "mock");
}

TEST(PluginGraft, GraftMultipleRoots) {
    Element root;
    root.type = "Window";
    root.framework = "win32";

    Element h1, h2;
    h1.type = "Window"; h1.framework = "win32"; h1.properties["hwnd"] = "0x1111";
    h1.bounds = {10, 20, 100, 100};
    h2.type = "Window"; h2.framework = "win32"; h2.properties["hwnd"] = "0x2222";
    h2.bounds = {200, 300, 100, 100};
    root.children = {h1, h2};

    s_mockJson = R"([
        {"target_hwnd":"0x1111","type":"Host1","children":[{"type":"A"}]},
        {"target_hwnd":"0x2222","type":"Host2","children":[{"type":"B"}]}
    ])";

    auto lp = make_mock_plugin();
    auto fw = make_mock_fw(&lp);
    enrich_with_plugin(root, nullptr, 0, fw);

    ASSERT_EQ(root.children[0].children.size(), 1);
    EXPECT_EQ(root.children[0].children[0].type, "A");
    ASSERT_EQ(root.children[1].children.size(), 1);
    EXPECT_EQ(root.children[1].children[0].type, "B");
}

TEST(PluginGraft, NestedChildren) {
    Element root;
    root.type = "Window";
    root.framework = "win32";
    root.properties["hwnd"] = "0x1000";
    root.bounds = {0, 0, 800, 600};

    s_mockJson = R"([{"target_hwnd":"0x1000","type":"Root","children":[
        {"type":"Parent","children":[
            {"type":"Child1","name":"hello"},
            {"type":"Child2","name":"world"}
        ]}
    ]}])";

    auto lp = make_mock_plugin();
    auto fw = make_mock_fw(&lp);
    enrich_with_plugin(root, nullptr, 0, fw);

    ASSERT_EQ(root.children.size(), 1);
    auto& parent = root.children[0];
    EXPECT_EQ(parent.type, "Parent");
    ASSERT_EQ(parent.children.size(), 2);
    EXPECT_EQ(parent.children[0].text, "hello");
    EXPECT_EQ(parent.children[1].text, "world");
}

TEST(PluginGraft, PropertiesCopied) {
    Element root;
    root.type = "Window";
    root.properties["hwnd"] = "0x1000";
    root.bounds = {0, 0, 100, 100};

    s_mockJson = R"([{"target_hwnd":"0x1000","type":"Root","children":[
        {"type":"Item","properties":{"visible":"true","role":"button"}}
    ]}])";

    auto lp = make_mock_plugin();
    auto fw = make_mock_fw(&lp);
    enrich_with_plugin(root, nullptr, 0, fw);

    ASSERT_EQ(root.children.size(), 1);
    EXPECT_EQ(root.children[0].properties["visible"], "true");
    EXPECT_EQ(root.children[0].properties["role"], "button");
}

TEST(PluginGraft, EmptyJsonReturnsFailure) {
    Element root;
    root.type = "Window";

    s_mockJson = nullptr;  // enrich returns 0

    auto lp = make_mock_plugin();
    auto fw = make_mock_fw(&lp);
    bool ok = enrich_with_plugin(root, nullptr, 0, fw);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(root.children.empty());
}

TEST(PluginGraft, InvalidJsonReturnsFailure) {
    Element root;
    root.type = "Window";

    s_mockJson = "this is not json{{{";

    auto lp = make_mock_plugin();
    auto fw = make_mock_fw(&lp);
    bool ok = enrich_with_plugin(root, nullptr, 0, fw);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(root.children.empty());
}

TEST(PluginGraft, DeepHwndMatch) {
    // target_hwnd is on a deeply nested element
    Element root;
    root.type = "Window"; root.framework = "win32";
    Element a, b, c;
    a.type = "A"; a.framework = "win32";
    b.type = "B"; b.framework = "win32";
    c.type = "C"; c.framework = "win32"; c.properties["hwnd"] = "0xDEEP";
    c.bounds = {50, 60, 200, 200};
    b.children.push_back(c);
    a.children.push_back(b);
    root.children.push_back(a);

    s_mockJson = R"([{"target_hwnd":"0xDEEP","type":"DeepHost","children":[
        {"type":"Leaf","name":"found it"}
    ]}])";

    auto lp = make_mock_plugin();
    auto fw = make_mock_fw(&lp);
    enrich_with_plugin(root, nullptr, 0, fw);

    // Navigate to the deeply nested C element
    auto& leaf = root.children[0].children[0].children[0];
    EXPECT_EQ(leaf.type, "C");
    ASSERT_EQ(leaf.children.size(), 1);
    EXPECT_EQ(leaf.children[0].type, "Leaf");
    EXPECT_EQ(leaf.children[0].text, "found it");
}

// ---- Bounds validation edge cases ----

TEST(PluginGraft, ZeroWidthHeightNoBounds) {
    // Elements with zero width or height should not have bounds set
    Element root;
    root.type = "Window";
    root.properties["hwnd"] = "0x1000";
    root.bounds = {0, 0, 800, 600};

    s_mockJson = R"([{"target_hwnd":"0x1000","type":"Root","children":[
        {"type":"Collapsed","name":"hidden","width":0,"height":0,"offsetX":10,"offsetY":20}
    ]}])";

    auto lp = make_mock_plugin();
    auto fw = make_mock_fw(&lp);
    enrich_with_plugin(root, nullptr, 0, fw);

    ASSERT_EQ(root.children.size(), 1);
    EXPECT_EQ(root.children[0].bounds.width, 0);
    EXPECT_EQ(root.children[0].bounds.height, 0);
    EXPECT_EQ(root.children[0].bounds.x, 0);
    EXPECT_EQ(root.children[0].bounds.y, 0);
}

TEST(PluginGraft, VeryLargeOffsetsClamped) {
    // Very large double offsets should be clamped to int range, not cause UB
    Element root;
    root.type = "Window";
    root.properties["hwnd"] = "0x1000";
    root.bounds = {0, 0, 800, 600};

    s_mockJson = R"([{"target_hwnd":"0x1000","type":"Root","children":[
        {"type":"Item","width":100,"height":50,"offsetX":3000000000.0,"offsetY":-3000000000.0}
    ]}])";

    auto lp = make_mock_plugin();
    auto fw = make_mock_fw(&lp);
    enrich_with_plugin(root, nullptr, 0, fw);

    ASSERT_EQ(root.children.size(), 1);
    auto& child = root.children[0];
    // Bounds should be set (width/height > 0) and values should be valid int range
    EXPECT_EQ(child.bounds.width, 100);
    EXPECT_EQ(child.bounds.height, 50);
    // The large offsets should be clamped, not undefined behavior
    EXPECT_TRUE(child.bounds.x > 0);   // clamped to INT_MAX
    EXPECT_TRUE(child.bounds.y < 0);   // clamped to INT_MIN
}

TEST(PluginGraft, NormalBoundsStillWork) {
    // Normal values should work exactly as before
    Element root;
    root.type = "Window";
    root.properties["hwnd"] = "0x1000";
    root.bounds = {100, 200, 800, 600};

    s_mockJson = R"([{"target_hwnd":"0x1000","type":"Root","children":[
        {"type":"Button","name":"OK","width":80,"height":30,"offsetX":10,"offsetY":20}
    ]}])";

    auto lp = make_mock_plugin();
    auto fw = make_mock_fw(&lp);
    enrich_with_plugin(root, nullptr, 0, fw);

    ASSERT_EQ(root.children.size(), 1);
    EXPECT_EQ(root.children[0].bounds.x, 110);   // 100 + 10
    EXPECT_EQ(root.children[0].bounds.y, 220);    // 200 + 20
    EXPECT_EQ(root.children[0].bounds.width, 80);
    EXPECT_EQ(root.children[0].bounds.height, 30);
}

// --- WIL result logger ---
// Guards the invariant Phase 2's MCP stdio transport depends on: WIL failure
// diagnostics go to stderr and are silent unless --debug is set.

namespace {

HRESULT failing_helper() {
    RETURN_HR(E_ACCESSDENIED);
}

} // namespace

TEST(WilResultLogger, SilentUnlessDebug) {
    lvt::install_wil_result_logger();

    const bool previous = lvt::g_debug;
    lvt::g_debug = false;

    testing::internal::CaptureStderr();
    testing::internal::CaptureStdout();
    EXPECT_EQ(failing_helper(), E_ACCESSDENIED);
    const auto out = testing::internal::GetCapturedStdout();
    const auto err = testing::internal::GetCapturedStderr();

    lvt::g_debug = previous;

    EXPECT_EQ(err.find("wil/"), std::string::npos);
    EXPECT_TRUE(out.empty());
}

TEST(WilResultLogger, ReportsToStderrNotStdoutWhenDebug) {
    lvt::install_wil_result_logger();

    const bool previous = lvt::g_debug;
    lvt::g_debug = true;

    testing::internal::CaptureStderr();
    testing::internal::CaptureStdout();
    EXPECT_EQ(failing_helper(), E_ACCESSDENIED);
    const auto out = testing::internal::GetCapturedStdout();
    const auto err = testing::internal::GetCapturedStderr();

    lvt::g_debug = previous;

    // The diagnostic names the originating site, and never touches stdout.
    EXPECT_NE(err.find("wil/return"), std::string::npos);
    EXPECT_NE(err.find("unit_tests.cpp"), std::string::npos);
    EXPECT_TRUE(out.empty());
}

// --- UIA property/view tables and RuntimeId handling ---

TEST(UiaView, ParsesAllThreeViews) {
    lvt::UiaView view = lvt::UiaView::raw;
    EXPECT_TRUE(lvt::parse_uia_view("control", view));
    EXPECT_EQ(view, lvt::UiaView::control);
    EXPECT_TRUE(lvt::parse_uia_view("RAW", view));
    EXPECT_EQ(view, lvt::UiaView::raw);
    EXPECT_TRUE(lvt::parse_uia_view("Content", view));
    EXPECT_EQ(view, lvt::UiaView::content);
}

TEST(UiaView, RejectsUnknownView) {
    lvt::UiaView view = lvt::UiaView::control;
    EXPECT_FALSE(lvt::parse_uia_view("tree", view));
    EXPECT_FALSE(lvt::parse_uia_view("", view));
    EXPECT_EQ(view, lvt::UiaView::control);  // unchanged on failure
}

TEST(UiaProps, ResolvesNamesLeniently) {
    const long expected = lvt::uia_property_id("AutomationId");
    EXPECT_NE(expected, 0);
    // Case-insensitive, and the UIA_/PropertyId decoration is optional, so a
    // name pasted from the docs resolves the same as one from lvt's output.
    EXPECT_EQ(lvt::uia_property_id("automationid"), expected);
    EXPECT_EQ(lvt::uia_property_id("UIA_AutomationIdPropertyId"), expected);
    EXPECT_EQ(lvt::uia_property_id("AutomationIdPropertyId"), expected);
}

TEST(UiaProps, UnknownNameResolvesToZero) {
    EXPECT_EQ(lvt::uia_property_id("NotARealProperty"), 0);
    EXPECT_EQ(lvt::uia_property_id(""), 0);
}

TEST(UiaProps, RoundTripsNameAndId) {
    for (long id : lvt::uia_core_property_ids()) {
        const auto name = lvt::uia_property_name(id);
        ASSERT_FALSE(name.empty()) << "property id " << id << " has no name";
        EXPECT_EQ(lvt::uia_property_id(name), id) << "round trip failed for " << name;
    }
}

TEST(UiaProps, PatternBackedPropertiesAreNamedUnderTheirPattern) {
    // The provider decides how to read a property from its name: anything
    // containing '.' is pattern-backed and is read with
    // GetCachedPropertyValueEx(ignoreDefaultValue=TRUE) so UIA reports
    // "not supported" instead of substituting a default (a Window otherwise
    // answers Toggle.ToggleState with 2 = Indeterminate). Core properties use
    // the plain form, because there "not supported" would also fire whenever a
    // provider simply did not set the value, dropping useful state such as
    // IsControlElement.
    //
    // That makes this naming convention load-bearing, so pin it down.
    const char* patternBacked[] = {
        "Toggle.ToggleState", "Value.Value", "Grid.RowCount",
        "ExpandCollapse.State", "RangeValue.Value", "Scroll.VerticalPercent",
        "SelectionItem.IsSelected", "Window.CanMaximize", "Transform.CanMove",
    };
    for (const char* name : patternBacked) {
        const std::string n = name;
        EXPECT_NE(n.find('.'), std::string::npos) << n << " must be pattern-namespaced";
        const long id = lvt::uia_property_id(n);
        EXPECT_NE(id, 0) << n << " does not resolve";
        EXPECT_EQ(lvt::uia_property_name(id), n);

        // The prefix must name a real pattern, or the convention is a lie.
        EXPECT_NE(lvt::uia_pattern_id(n.substr(0, n.find('.'))), 0)
            << n << " has no matching pattern";
    }

    // Conversely, identity/state properties must NOT look pattern-backed.
    for (const char* name : {"AutomationId", "ControlType", "IsEnabled",
                             "IsControlElement", "IsContentElement", "RuntimeId"}) {
        const std::string n = name;
        EXPECT_EQ(n.find('.'), std::string::npos) << n << " must not be pattern-namespaced";
        EXPECT_NE(lvt::uia_property_id(n), 0) << n << " does not resolve";
    }
}

TEST(UiaProps, EveryDottedCorePropertyNamesARealPattern) {
    // Guards the same rule across the whole default set, so a property added
    // later cannot quietly break the read path.
    for (long id : lvt::uia_core_property_ids()) {
        const auto name = lvt::uia_property_name(id);
        const auto dot = name.find('.');
        if (dot == std::string::npos)
            continue;
        EXPECT_NE(lvt::uia_pattern_id(name.substr(0, dot)), 0)
            << name << " is pattern-namespaced but '" << name.substr(0, dot)
            << "' is not a known pattern";
    }
}

TEST(UiaProps, SuppressesFrameworkSpecificUnsetSentinels) {
    // Win32 reports 0 for an unset Level; XAML reports -1. Both are noise.
    const long level = lvt::uia_property_id("Level");
    EXPECT_TRUE(lvt::uia_property_value_is_unset(level, "0"));
    EXPECT_TRUE(lvt::uia_property_value_is_unset(level, "-1"));
    EXPECT_FALSE(lvt::uia_property_value_is_unset(level, "2"));

    const long automationId = lvt::uia_property_id("AutomationId");
    EXPECT_FALSE(lvt::uia_property_value_is_unset(automationId, "0"));
}

TEST(UiaProps, ResolvesPatternNamesLeniently) {
    const long invoke = lvt::uia_pattern_id("Invoke");
    EXPECT_NE(invoke, 0);
    EXPECT_EQ(lvt::uia_pattern_id("invoke"), invoke);
    EXPECT_EQ(lvt::uia_pattern_id("InvokePattern"), invoke);
    EXPECT_EQ(lvt::uia_pattern_name(invoke), "Invoke");
    EXPECT_EQ(lvt::uia_pattern_id("NotAPattern"), 0);
}

TEST(UiaProps, NamesKnownControlTypesAndFallsBackForUnknown) {
    EXPECT_EQ(lvt::uia_control_type_name(UIA_ButtonControlTypeId), "Button");
    EXPECT_EQ(lvt::uia_control_type_name(UIA_EditControlTypeId), "Edit");
    // An unrecognised control type must stay identifiable rather than vanish.
    EXPECT_EQ(lvt::uia_control_type_name(999999), "ControlType(999999)");
}

// --- Architecture reporting ---
// --uia skips the architecture-match check because UIA needs no injection, so
// the check has to classify targets correctly for that exemption to mean
// anything. x86 was previously missing here, which silently disabled the check
// for every 32-bit target.

TEST(Architecture, NamesEveryValue) {
    EXPECT_STREQ(lvt::architecture_name(lvt::Architecture::x64), "x64");
    EXPECT_STREQ(lvt::architecture_name(lvt::Architecture::arm64), "arm64");
    EXPECT_STREQ(lvt::architecture_name(lvt::Architecture::x86), "x86");
    EXPECT_STREQ(lvt::architecture_name(lvt::Architecture::unknown), "unknown");
}

TEST(Architecture, HostIsIdentifiedNotUnknown) {
    // An x86 build used to report its own architecture as "unknown", which made
    // every comparison against it meaningless.
    EXPECT_NE(lvt::get_host_architecture(), lvt::Architecture::unknown);
}

TEST(Architecture, CurrentProcessMatchesHost) {
    EXPECT_EQ(lvt::detect_process_architecture(GetCurrentProcessId()),
              lvt::get_host_architecture());
}

TEST(UiaRuntimeId, FormatsAndParsesRoundTrip) {
    const std::vector<int> id{42, 3150138, 4, 5};
    const auto text = lvt::format_runtime_id(id);
    EXPECT_EQ(text, "42.3150138.4.5");

    std::vector<int> parsed;
    ASSERT_TRUE(lvt::parse_runtime_id(text, parsed));
    EXPECT_EQ(parsed, id);
}

TEST(UiaRuntimeId, RoundTripsNegativeComponents) {
    // RuntimeIds derived from an HWND with the high bit set have negative
    // components, and format_runtime_id emits them with a '-'. The parser must
    // accept what the formatter produces, or a uia:<RuntimeId> reference to
    // such an element silently resolves to nothing.
    const std::vector<int> id{42, -2147483647, 9705532, -2, 0};
    const auto text = lvt::format_runtime_id(id);
    EXPECT_EQ(text, "42.-2147483647.9705532.-2.0");

    std::vector<int> parsed;
    ASSERT_TRUE(lvt::parse_runtime_id(text, parsed)) << text;
    EXPECT_EQ(parsed, id);
}

TEST(UiaRuntimeId, RejectsMalformedInput) {
    std::vector<int> parsed;
    EXPECT_FALSE(lvt::parse_runtime_id("", parsed));
    EXPECT_FALSE(lvt::parse_runtime_id("42..5", parsed));
    EXPECT_FALSE(lvt::parse_runtime_id("42.", parsed));
    EXPECT_FALSE(lvt::parse_runtime_id("4a.5", parsed));
    EXPECT_FALSE(lvt::parse_runtime_id("-", parsed));
    EXPECT_FALSE(lvt::parse_runtime_id("1.-", parsed));
    EXPECT_FALSE(lvt::parse_runtime_id("1..2", parsed));
}

TEST(UiaProperties, DoubleFormattingRoundTripsAdjacentValues) {
    const double first = 1.2345678901234567;
    const double second =
        std::nextafter(first, std::numeric_limits<double>::infinity());
    const auto firstText = format_uia_double(first);
    const auto secondText = format_uia_double(second);
    EXPECT_EQ(std::stod(firstText), first);
    EXPECT_EQ(std::stod(secondText), second);
    EXPECT_NE(firstText, secondText);
}

TEST(UiaProperties, RangeReadbackFailureHasNoSuccessShapedValue) {
    const auto failed = uia_range_readback_result(E_FAIL, 0);
    EXPECT_FALSE(failed.ok);
    EXPECT_FALSE(failed.hasValue);
    EXPECT_TRUE(failed.value.empty());
    EXPECT_EQ(failed.hresult, E_FAIL);
    EXPECT_NE(failed.error.find("actual value is unknown"), std::string::npos);

    const auto nonFinite = uia_range_readback_result(
        S_OK, std::numeric_limits<double>::infinity());
    EXPECT_FALSE(nonFinite.ok);
    EXPECT_FALSE(nonFinite.hasValue);

    const auto succeeded =
        uia_range_readback_result(S_OK, 12.345678901234567);
    EXPECT_TRUE(succeeded.ok);
    EXPECT_TRUE(succeeded.hasValue);
    EXPECT_EQ(std::stod(succeeded.value), 12.345678901234567);
}

TEST(UiaProperties, OneShotFallbackTreeReceivesConnectionOwnedIdentities) {
    UiaPropertyIdentityCache identities;
    Element root;
    root.type = "Window";
    root.framework = "uia";
    root.properties["RuntimeId"] = "42.100";
    Element child;
    child.type = "Edit";
    child.framework = "uia";
    child.properties["AutomationId"] = "RawOnlyInput";
    child.properties["RuntimeId"] = "42.100.7";
    root.children.push_back(child);

    identities.attach(root);
    ASSERT_NE(root.children[0].providerHandle, 0u);
    const auto originalHandle = root.children[0].providerHandle;
    assign_element_keys(root);
    identities.remember(root);

    std::string error;
    const auto byKey =
        identities.resolve(root.children[0].key, error);
    ASSERT_TRUE(byKey.has_value()) << error;
    EXPECT_EQ(*byKey, originalHandle);
    const auto byRuntime =
        identities.resolve("uia:42.100.7", error);
    ASSERT_TRUE(byRuntime.has_value()) << error;
    EXPECT_EQ(*byRuntime, originalHandle);

    Element fallbackChild = child;
    fallbackChild.providerHandle = 0;
    identities.attach(fallbackChild);
    EXPECT_EQ(fallbackChild.providerHandle, originalHandle);

    Element conflicting = child;
    conflicting.key = root.children[0].key;
    conflicting.properties["RuntimeId"] = "42.100.8";
    identities.attach(conflicting);
    identities.remember(conflicting);
    error.clear();
    EXPECT_FALSE(identities.resolve(conflicting.key, error).has_value());
    EXPECT_NE(error.find("ambiguous"), std::string::npos);
}

TEST(UiaProperties, IdentityCachePrunesVirtualizationChurnAndRejectsStaleAliases) {
    UiaPropertyIdentityCache identities;
    std::string firstKey;
    uint64_t firstHandle = 0;
    std::string previousKey;

    for (int generation = 0; generation < 1000; ++generation) {
        Element root;
        root.type = "Window";
        root.framework = "uia";
        root.properties["RuntimeId"] = "42.500";
        Element item;
        item.type = "ListItem";
        item.framework = "uia";
        item.properties["AutomationId"] =
            "item-" + std::to_string(generation);
        item.properties["RuntimeId"] =
            "42.500." + std::to_string(generation);
        root.children.push_back(item);
        identities.attach(root);
        assign_element_keys(root);
        identities.remember(root);
        if (generation == 0) {
            firstKey = root.children[0].key;
            firstHandle = root.children[0].providerHandle;
        }
        if (generation == 998)
            previousKey = root.children[0].key;
        EXPECT_LE(identities.runtime_id_count(), 3u);
        EXPECT_LE(identities.key_alias_count(), 3u);
    }

    std::string error;
    EXPECT_TRUE(identities.resolve(previousKey, error).has_value())
        << "the immediately previous snapshot alias should survive one patch: "
        << error;
    EXPECT_FALSE(identities.runtime_id(firstHandle).has_value());
    error.clear();
    EXPECT_FALSE(identities.resolve(firstKey, error).has_value());
    EXPECT_NE(error.find("stale"), std::string::npos);
}

TEST(UiaProperties, TruncatedSnapshotsDoNotPruneKnownIdentities) {
    UiaPropertyIdentityCache identities;
    Element complete;
    complete.type = "Edit";
    complete.framework = "uia";
    complete.properties["RuntimeId"] = "42.700.1";
    identities.attach(complete);
    const auto handle = complete.providerHandle;

    for (int i = 0; i < 10; ++i) {
        Element partial;
        partial.type = "Edit";
        partial.framework = "uia";
        partial.properties["RuntimeId"] =
            "42.700." + std::to_string(i + 2);
        identities.attach(partial, "default", false);
    }
    EXPECT_EQ(identities.runtime_id(handle), "42.700.1");
}

TEST(UiaProperties, RawIdentitySurvivesControlAndContentWalks) {
    UiaPropertyIdentityCache identities;
    Element rawOnly;
    rawOnly.type = "Edit";
    rawOnly.framework = "uia";
    rawOnly.properties["AutomationId"] = "RawOnly";
    rawOnly.properties["RuntimeId"] = "42.800.1";
    ASSERT_TRUE(identities.attach(rawOnly, "raw"));
    assign_element_keys(rawOnly);
    ASSERT_TRUE(identities.remember(rawOnly, "raw"));
    const auto rawKey = rawOnly.key;
    const auto rawHandle = rawOnly.providerHandle;

    for (int i = 0; i < 20; ++i) {
        Element control;
        control.type = "Button";
        control.framework = "uia";
        control.properties["RuntimeId"] =
            "42.801." + std::to_string(i);
        ASSERT_TRUE(identities.attach(control, "control"));
        assign_element_keys(control);
        ASSERT_TRUE(identities.remember(control, "control"));

        Element content;
        content.type = "Text";
        content.framework = "uia";
        content.properties["RuntimeId"] =
            "42.802." + std::to_string(i);
        ASSERT_TRUE(identities.attach(content, "content"));
        assign_element_keys(content);
        ASSERT_TRUE(identities.remember(content, "content"));
    }

    std::string error;
    const auto resolved = identities.resolve(rawKey, error);
    ASSERT_TRUE(resolved.has_value()) << error;
    EXPECT_EQ(*resolved, rawHandle);
}

TEST(UiaProperties, OversizedSnapshotIsRefusedBeforeReturningInvalidKeys) {
    UiaPropertyIdentityCache identities;
    Element root;
    root.type = "Window";
    root.framework = "uia";
    root.children.reserve(
        UiaPropertyIdentityCache::kMaximumRuntimeIds + 1);
    for (size_t i = 0;
         i <= UiaPropertyIdentityCache::kMaximumRuntimeIds;
         ++i) {
        Element child;
        child.type = "Text";
        child.framework = "uia";
        child.properties["RuntimeId"] =
            "42.900." + std::to_string(i);
        root.children.push_back(std::move(child));
    }
    EXPECT_FALSE(identities.attach(root, "raw"));
    EXPECT_EQ(identities.runtime_id_count(), 0u);
    EXPECT_EQ(identities.key_alias_count(), 0u);

    root.children.pop_back();
    ASSERT_TRUE(identities.attach(root, "raw"));
    const auto protectedHandle = root.children.front().providerHandle;
    Element additional;
    additional.type = "Text";
    additional.framework = "uia";
    additional.properties["RuntimeId"] = "42.999.1";
    EXPECT_FALSE(identities.attach(additional, "control"));
    EXPECT_TRUE(identities.runtime_id(protectedHandle).has_value())
        << "capacity handling evicted an identity from the current raw snapshot";

    UiaPropertyIdentityCache scoped;
    Element one;
    one.type = "Text";
    one.framework = "uia";
    one.properties["RuntimeId"] = "42.901.1";
    for (size_t i = 0;
         i < UiaPropertyIdentityCache::kMaximumScopes;
         ++i) {
        EXPECT_TRUE(scoped.attach(
            one, "scope-" + std::to_string(i)));
    }
    EXPECT_FALSE(scoped.attach(one, "one-scope-too-many"));
}

TEST(FindElementByRef, ResolvesUiaRuntimeIdReference) {
    lvt::Element root;
    root.type = "Window";
    root.properties["RuntimeId"] = "42.100";
    lvt::Element child;
    child.type = "Button";
    child.properties["RuntimeId"] = "42.100.3.7";
    root.children.push_back(child);
    lvt::assign_element_ids(root);

    auto* found = lvt::find_element_by_ref(root, "uia:42.100.3.7");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->type, "Button");

    EXPECT_EQ(lvt::find_element_by_ref(root, "uia:42.999"), nullptr);
    // A plain id must still win; the uia: prefix is what selects RuntimeId lookup.
    auto* byId = lvt::find_element_by_ref(root, "e0");
    ASSERT_NE(byId, nullptr);
    EXPECT_EQ(byId->type, "Window");
}

// --- Enum-valued property rendering ---
// UIA returns these as bare integers. Every enum has its own value space, and
// several overlap numerically (ToggleState_Indeterminate == 2 ==
// WindowInteractionState_ReadyForUserInteraction == RowOrColumnMajor_Indeterminate),
// so a mapping keyed on the wrong property is silently plausible. These tests
// pin each enum against the SDK constants rather than against literals.

TEST(UiaEnums, ToggleStateCoversEveryValue) {
    const long id = lvt::uia_property_id("Toggle.ToggleState");
    EXPECT_TRUE(lvt::uia_property_is_enum(id));
    EXPECT_EQ(lvt::uia_enum_value_name(id, ToggleState_Off), "Off");
    EXPECT_EQ(lvt::uia_enum_value_name(id, ToggleState_On), "On");
    EXPECT_EQ(lvt::uia_enum_value_name(id, ToggleState_Indeterminate), "Indeterminate");
}

TEST(UiaEnums, ExpandCollapseStateCoversEveryValue) {
    const long id = lvt::uia_property_id("ExpandCollapse.State");
    EXPECT_TRUE(lvt::uia_property_is_enum(id));
    EXPECT_EQ(lvt::uia_enum_value_name(id, ExpandCollapseState_Collapsed), "Collapsed");
    EXPECT_EQ(lvt::uia_enum_value_name(id, ExpandCollapseState_Expanded), "Expanded");
    EXPECT_EQ(lvt::uia_enum_value_name(id, ExpandCollapseState_PartiallyExpanded),
              "PartiallyExpanded");
    EXPECT_EQ(lvt::uia_enum_value_name(id, ExpandCollapseState_LeafNode), "LeafNode");
}

TEST(UiaEnums, OrientationCoversEveryValue) {
    const long id = lvt::uia_property_id("Orientation");
    EXPECT_TRUE(lvt::uia_property_is_enum(id));
    EXPECT_EQ(lvt::uia_enum_value_name(id, OrientationType_None), "None");
    EXPECT_EQ(lvt::uia_enum_value_name(id, OrientationType_Horizontal), "Horizontal");
    EXPECT_EQ(lvt::uia_enum_value_name(id, OrientationType_Vertical), "Vertical");
}

TEST(UiaEnums, WindowVisualStateCoversEveryValue) {
    const long id = lvt::uia_property_id("Window.WindowVisualState");
    EXPECT_TRUE(lvt::uia_property_is_enum(id));
    EXPECT_EQ(lvt::uia_enum_value_name(id, WindowVisualState_Normal), "Normal");
    EXPECT_EQ(lvt::uia_enum_value_name(id, WindowVisualState_Maximized), "Maximized");
    EXPECT_EQ(lvt::uia_enum_value_name(id, WindowVisualState_Minimized), "Minimized");
}

TEST(UiaEnums, WindowInteractionStateCoversEveryValue) {
    // Previously emitted raw, so a Window reported WindowInteractionState="2".
    const long id = lvt::uia_property_id("Window.WindowInteractionState");
    EXPECT_TRUE(lvt::uia_property_is_enum(id));
    EXPECT_EQ(lvt::uia_enum_value_name(id, WindowInteractionState_Running), "Running");
    EXPECT_EQ(lvt::uia_enum_value_name(id, WindowInteractionState_Closing), "Closing");
    EXPECT_EQ(lvt::uia_enum_value_name(id, WindowInteractionState_ReadyForUserInteraction),
              "ReadyForUserInteraction");
    EXPECT_EQ(lvt::uia_enum_value_name(id, WindowInteractionState_BlockedByModalWindow),
              "BlockedByModalWindow");
    EXPECT_EQ(lvt::uia_enum_value_name(id, WindowInteractionState_NotResponding),
              "NotResponding");
}

TEST(UiaEnums, RowOrColumnMajorCoversEveryValue) {
    const long id = lvt::uia_property_id("Table.RowOrColumnMajor");
    EXPECT_TRUE(lvt::uia_property_is_enum(id));
    EXPECT_EQ(lvt::uia_enum_value_name(id, RowOrColumnMajor_RowMajor), "RowMajor");
    EXPECT_EQ(lvt::uia_enum_value_name(id, RowOrColumnMajor_ColumnMajor), "ColumnMajor");
    EXPECT_EQ(lvt::uia_enum_value_name(id, RowOrColumnMajor_Indeterminate), "Indeterminate");
}

TEST(UiaEnums, LiveSettingCoversEveryValue) {
    const long id = lvt::uia_property_id("LiveSetting");
    EXPECT_TRUE(lvt::uia_property_is_enum(id));
    EXPECT_EQ(lvt::uia_enum_value_name(id, Off), "Off");
    EXPECT_EQ(lvt::uia_enum_value_name(id, Polite), "Polite");
    EXPECT_EQ(lvt::uia_enum_value_name(id, Assertive), "Assertive");
}

TEST(UiaEnums, LandmarkTypeCoversEveryValue) {
    // Landmark ids live in the 80000 range, far from every other enum, which is
    // exactly why a copy-paste mistake here would be invisible in normal output.
    const long id = lvt::uia_property_id("LandmarkType");
    EXPECT_TRUE(lvt::uia_property_is_enum(id));
    EXPECT_EQ(lvt::uia_enum_value_name(id, UIA_CustomLandmarkTypeId), "Custom");
    EXPECT_EQ(lvt::uia_enum_value_name(id, UIA_FormLandmarkTypeId), "Form");
    EXPECT_EQ(lvt::uia_enum_value_name(id, UIA_MainLandmarkTypeId), "Main");
    EXPECT_EQ(lvt::uia_enum_value_name(id, UIA_NavigationLandmarkTypeId), "Navigation");
    EXPECT_EQ(lvt::uia_enum_value_name(id, UIA_SearchLandmarkTypeId), "Search");
}

TEST(UiaEnums, OverlappingNumericValuesResolvePerProperty) {
    // 2 means something different in each of these. A mapping keyed on the
    // wrong property would still produce a plausible-looking name, so assert
    // the disambiguation directly.
    EXPECT_EQ(lvt::uia_enum_value_name(lvt::uia_property_id("Toggle.ToggleState"), 2),
              "Indeterminate");
    EXPECT_EQ(lvt::uia_enum_value_name(
                  lvt::uia_property_id("Window.WindowInteractionState"), 2),
              "ReadyForUserInteraction");
    EXPECT_EQ(lvt::uia_enum_value_name(lvt::uia_property_id("Orientation"), 2), "Vertical");
    EXPECT_EQ(lvt::uia_enum_value_name(lvt::uia_property_id("Window.WindowVisualState"), 2),
              "Minimized");
    EXPECT_EQ(lvt::uia_enum_value_name(lvt::uia_property_id("ExpandCollapse.State"), 2),
              "PartiallyExpanded");
    EXPECT_EQ(lvt::uia_enum_value_name(lvt::uia_property_id("Table.RowOrColumnMajor"), 2),
              "Indeterminate");
}

TEST(UiaEnums, UnknownValuesNameTheirEnum) {
    // The value is unrecognised, so this is where naming the enum earns its
    // keep: a bare "99" would say neither what it means nor that lvt failed to
    // recognise it. Known values stay unqualified because the property key
    // already names the enum.
    EXPECT_EQ(lvt::uia_enum_value_name(lvt::uia_property_id("Toggle.ToggleState"), 99),
              "ToggleState(99)");
    EXPECT_EQ(lvt::uia_enum_value_name(
                  lvt::uia_property_id("Window.WindowInteractionState"), 99),
              "WindowInteractionState(99)");
    EXPECT_EQ(lvt::uia_enum_value_name(lvt::uia_property_id("LandmarkType"), 99),
              "LandmarkType(99)");
    EXPECT_EQ(lvt::uia_control_type_name(999999), "ControlType(999999)");
}

TEST(UiaEnums, NonEnumPropertiesAreNotTreatedAsEnums) {
    for (const char* name : {"AutomationId", "Name", "ProcessId", "Level",
                             "PositionInSet", "SizeOfSet", "IsEnabled",
                             "Value.Value", "Grid.RowCount"}) {
        const long id = lvt::uia_property_id(name);
        ASSERT_NE(id, 0) << name;
        EXPECT_FALSE(lvt::uia_property_is_enum(id)) << name << " is not an enum";
        EXPECT_TRUE(lvt::uia_enum_value_name(id, 1).empty()) << name;
    }
}

TEST(UiaEnums, EnumTableAndEmittedSetAgreeBothWays) {
    const auto& core = lvt::uia_core_property_ids();
    const auto& enums = lvt::uia_enum_property_ids();

    // Forward: every enum mapping must belong to a property lvt actually
    // emits, or it is dead weight.
    for (long id : enums) {
        EXPECT_NE(std::find(core.begin(), core.end(), id), core.end())
            << lvt::uia_property_name(id) << " is enum-mapped but never emitted";
        EXPECT_TRUE(lvt::uia_property_is_enum(id));
    }

    // Reverse: every emitted property whose name is a known enum property must
    // be in the enum table, or it leaks a raw integer. Driven off the tables
    // themselves rather than a hardcoded list, so adding either side is caught.
    for (long id : core) {
        const auto name = lvt::uia_property_name(id);
        const bool mapped = std::find(enums.begin(), enums.end(), id) != enums.end();
        EXPECT_EQ(mapped, lvt::uia_property_is_enum(id))
            << name << " disagrees between the enum table and uia_property_is_enum";
    }
}

TEST(UiaEnums, EnumPropertiesNeverCollideWithUnsetSentinels) {
    // The sentinel table is compared against the *raw* UIA value, before
    // humanizing. Expressing a sentinel in humanized form makes it dead: it
    // shipped once as LandmarkType="LandmarkType(0)" and LiveSetting="Off" on
    // every element. A sentinel that parses as an integer is raw-form.
    for (long id : lvt::uia_enum_property_ids()) {
        for (const char* candidate : {"0", "-1", "1", "2"}) {
            if (!lvt::uia_property_value_is_unset(id, candidate))
                continue;
            // If a sentinel exists it must be numeric, i.e. the raw form, not
            // the name this enum would humanize it into.
            const auto humanized = lvt::uia_enum_value_name(id, std::atol(candidate));
            EXPECT_NE(humanized, candidate)
                << lvt::uia_property_name(id)
                << " sentinel is written in humanized form and can never match";
        }
    }
}

TEST(UiaCulture, RendersLcidAsLocaleName) {
    // "Culture=1033" is opaque; "Culture=en-US" is not.
    EXPECT_EQ(lvt::uia_culture_name(1033), "en-US");
    EXPECT_EQ(lvt::uia_culture_name(1036), "fr-FR");
}

TEST(XamlPropertyFilter, ZeroIsCapturedForStateProperties) {
    // Windows.UI.Xaml.Visibility::Visible and Orientation::Vertical are both
    // 0. A blanket "0 means unset" filter would drop exactly the common case
    // for these two, silently, while leaving Collapsed/Horizontal untouched.
    EXPECT_TRUE(lvt::xaml_should_capture_property(L"Visibility", L"0", L"Enum"));
    EXPECT_TRUE(lvt::xaml_should_capture_property(L"Orientation", L"0", L"Enum"));
    EXPECT_TRUE(lvt::xaml_should_capture_property(L"IsChecked", L"0", L"Boolean"));
}

TEST(XamlPropertyFilter, EmptyValueIsNeverCaptured) {
    // Empty string is the only "nothing came back" signal this API gives us.
    EXPECT_FALSE(lvt::xaml_should_capture_property(L"Visibility", L"", L"Enum"));
    EXPECT_FALSE(lvt::xaml_should_capture_property(L"Text", L"", L"String"));
}

TEST(XamlPropertyFilter, ArbitraryPropertiesAreCapturedWithRecognizedValueTypes) {
    // Broadened capture: any named property is captured (not just a curated
    // text/state allowlist) as long as its ValueType is a recognized
    // primitive shape — this is what makes the property panel show a
    // control's full property set (FontSize, Opacity, a custom DP, ...)
    // rather than a handful of hand-picked names.
    EXPECT_TRUE(lvt::xaml_should_capture_property(L"FontSize", L"14", L"Double"));
    EXPECT_TRUE(lvt::xaml_should_capture_property(L"Opacity", L"0.5", L"Double"));
    EXPECT_TRUE(lvt::xaml_should_capture_property(L"SomeRandomProperty", L"42", L"Int32"));
}

TEST(TypedPropertyContract, EditorKindUsesDeclaredPropertyType) {
    EXPECT_EQ(
        classify_property_editor("String", true),
        PropertyEditorKind::string);
    EXPECT_EQ(
        classify_property_editor("System.Boolean", true),
        PropertyEditorKind::boolean);
    EXPECT_EQ(
        classify_property_editor("Windows.Foundation.Int32", true),
        PropertyEditorKind::integer);
    EXPECT_EQ(
        classify_property_editor("Double", true),
        PropertyEditorKind::number);
    EXPECT_EQ(
        classify_property_editor("Enum", true),
        PropertyEditorKind::enumeration);
    EXPECT_EQ(
        classify_property_editor("String", false),
        PropertyEditorKind::readonly);
}

TEST(TypedPropertyContract, EditorKindWireNamesAreProviderNeutral) {
    EXPECT_STREQ(
        property_editor_kind_name(PropertyEditorKind::readonly), "readonly");
    EXPECT_STREQ(
        property_editor_kind_name(PropertyEditorKind::string), "string");
    EXPECT_STREQ(
        property_editor_kind_name(PropertyEditorKind::boolean), "boolean");
    EXPECT_STREQ(
        property_editor_kind_name(PropertyEditorKind::integer), "integer");
    EXPECT_STREQ(
        property_editor_kind_name(PropertyEditorKind::number), "number");
    EXPECT_STREQ(
        property_editor_kind_name(PropertyEditorKind::enumeration), "enum");
    EXPECT_STREQ(
        property_editor_kind_name(PropertyEditorKind::command), "command");
}

namespace {

Element editable_uia_element(
    const std::string& type, const std::string& patterns,
    std::initializer_list<std::pair<const std::string, std::string>> properties) {
    Element element;
    element.type = type;
    element.framework = "uia";
    element.properties["SupportedPatterns"] = patterns;
    element.properties["IsEnabled"] = "true";
    for (const auto& [name, value] : properties)
        element.properties[name] = value;
    return element;
}

const PropertyDescriptor* descriptor_named(
    const PropertySchema& schema, const std::string& name) {
    for (const auto& descriptor : schema.descriptors) {
        if (descriptor.name == name)
            return &descriptor;
    }
    return nullptr;
}

const PropertyValue* value_for(
    const PropertySnapshotResult& snapshot, const PropertyDescriptor& descriptor) {
    for (const auto& value : snapshot.values) {
        if (value.descriptorId == descriptor.descriptorId)
            return &value;
    }
    return nullptr;
}

std::vector<std::string> choice_values(const PropertyDescriptor& descriptor) {
    std::vector<std::string> result;
    for (const auto& choice : descriptor.choices)
        result.push_back(choice.value);
    return result;
}

} // namespace

TEST(UiaTypedProperties, ValueHonorsReadOnlyAndSchemaCacheDoesNotStoreValues) {
    UiaPropertySchemaCache cache;
    auto writable = editable_uia_element(
        "Edit", "Value",
        {{"Value.Value", "first"}, {"Value.IsReadOnly", "false"}});
    auto first = make_uia_property_snapshot(
        writable, UiaSelectionCapabilities{}, cache);
    ASSERT_TRUE(first.ok);
    const auto* descriptor = descriptor_named(*first.schema, "Value.Value");
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->kind, PropertyEditorKind::string);
    EXPECT_TRUE(descriptor->writable);
    EXPECT_FALSE(descriptor->supportsClear);
    const auto* firstValue = value_for(first, *descriptor);
    ASSERT_NE(firstValue, nullptr);
    EXPECT_EQ(firstValue->value, "first");

    writable.properties["Value.Value"] = "second";
    auto second = make_uia_property_snapshot(
        writable, UiaSelectionCapabilities{}, cache);
    EXPECT_EQ(first.schema.get(), second.schema.get());
    EXPECT_EQ(first.schema->schemaId, second.schema->schemaId);
    const auto* secondValue = value_for(second, *descriptor);
    ASSERT_NE(secondValue, nullptr);
    EXPECT_EQ(secondValue->value, "second");
    EXPECT_EQ(cache.size(), 1u);

    writable.properties["Value.IsReadOnly"] = "true";
    auto readOnly = make_uia_property_snapshot(
        writable, UiaSelectionCapabilities{}, cache);
    const auto* readOnlyDescriptor =
        descriptor_named(*readOnly.schema, "Value.Value");
    ASSERT_NE(readOnlyDescriptor, nullptr);
    EXPECT_EQ(readOnlyDescriptor->kind, PropertyEditorKind::readonly);
    EXPECT_FALSE(readOnlyDescriptor->writable);
    EXPECT_NE(readOnly.schema->schemaId, first.schema->schemaId);
    EXPECT_EQ(cache.size(), 2u);
}

TEST(UiaTypedProperties, RangeValueSuppliesBoundsWithoutInventingStep) {
    UiaPropertySchemaCache cache;
    auto slider = editable_uia_element(
        "Slider", "RangeValue",
        {{"RangeValue.Value", "25"},
         {"RangeValue.Minimum", "0"},
         {"RangeValue.Maximum", "100"},
         {"RangeValue.IsReadOnly", "false"}});
    auto snapshot = make_uia_property_snapshot(
        slider, UiaSelectionCapabilities{}, cache);
    const auto* descriptor =
        descriptor_named(*snapshot.schema, "RangeValue.Value");
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->kind, PropertyEditorKind::number);
    ASSERT_TRUE(descriptor->minimum.has_value());
    ASSERT_TRUE(descriptor->maximum.has_value());
    EXPECT_DOUBLE_EQ(*descriptor->minimum, 0);
    EXPECT_DOUBLE_EQ(*descriptor->maximum, 100);
    EXPECT_FALSE(descriptor->step.has_value());

    slider.properties["RangeValue.IsReadOnly"] = "true";
    auto readOnly = make_uia_property_snapshot(
        slider, UiaSelectionCapabilities{}, cache);
    const auto* readOnlyDescriptor =
        descriptor_named(*readOnly.schema, "RangeValue.Value");
    ASSERT_NE(readOnlyDescriptor, nullptr);
    EXPECT_EQ(readOnlyDescriptor->kind, PropertyEditorKind::readonly);
    EXPECT_FALSE(readOnlyDescriptor->writable);
    EXPECT_EQ(readOnlyDescriptor->minimum, descriptor->minimum);
    EXPECT_EQ(readOnlyDescriptor->maximum, descriptor->maximum);

    slider.properties["RangeValue.Minimum"] = "0.12345678901234566";
    slider.properties["RangeValue.Maximum"] = "0.12345678901239999";
    slider.properties["RangeValue.IsReadOnly"] = "false";
    auto precise = make_uia_property_snapshot(
        slider, UiaSelectionCapabilities{}, cache);
    const auto* preciseDescriptor =
        descriptor_named(*precise.schema, "RangeValue.Value");
    ASSERT_NE(preciseDescriptor, nullptr);
    ASSERT_TRUE(preciseDescriptor->minimum.has_value());
    ASSERT_TRUE(preciseDescriptor->maximum.has_value());
    EXPECT_EQ(*preciseDescriptor->minimum,
              std::stod("0.12345678901234566"));
    EXPECT_EQ(*preciseDescriptor->maximum,
              std::stod("0.12345678901239999"));
    EXPECT_NE(precise.schema->schemaId, snapshot.schema->schemaId);
}

TEST(UiaTypedProperties, ToggleAndExpandExposeOnlyDeterministicStates) {
    UiaPropertySchemaCache cache;
    auto element = editable_uia_element(
        "CheckBox", "Toggle,ExpandCollapse",
        {{"Toggle.ToggleState", "Indeterminate"},
         {"ExpandCollapse.State", "PartiallyExpanded"}});
    auto snapshot = make_uia_property_snapshot(
        element, UiaSelectionCapabilities{}, cache);

    const auto* toggle =
        descriptor_named(*snapshot.schema, "Toggle.ToggleState");
    ASSERT_NE(toggle, nullptr);
    EXPECT_EQ(toggle->kind, PropertyEditorKind::enumeration);
    EXPECT_EQ(choice_values(*toggle),
              (std::vector<std::string>{"Off", "On"}));

    const auto* expand =
        descriptor_named(*snapshot.schema, "ExpandCollapse.State");
    ASSERT_NE(expand, nullptr);
    EXPECT_EQ(expand->kind, PropertyEditorKind::enumeration);
    EXPECT_EQ(choice_values(*expand),
              (std::vector<std::string>{"Expanded", "Collapsed"}));

    element.properties["ExpandCollapse.State"] = "LeafNode";
    auto leaf = make_uia_property_snapshot(
        element, UiaSelectionCapabilities{}, cache);
    const auto* leafExpand =
        descriptor_named(*leaf.schema, "ExpandCollapse.State");
    ASSERT_NE(leafExpand, nullptr);
    EXPECT_EQ(leafExpand->kind, PropertyEditorKind::readonly);
    EXPECT_FALSE(leafExpand->writable);
}

TEST(UiaTypedProperties, SelectionAndScrollCommandsFollowProviderCapabilities) {
    UiaPropertySchemaCache cache;
    UiaSelectionCapabilities selection{
        .known = true,
        .canSelectMultiple = true,
        .isSelectionRequired = false,
    };
    auto item = editable_uia_element(
        "ListItem", "SelectionItem,Scroll",
        {{"SelectionItem.IsSelected", "false"},
         {"Scroll.HorizontallyScrollable", "false"},
         {"Scroll.VerticallyScrollable", "true"},
         {"Scroll.VerticalPercent", "10"}});
    auto snapshot = make_uia_property_snapshot(item, selection, cache);

    const auto* selectionDescriptor =
        descriptor_named(*snapshot.schema, "SelectionItem.Action");
    ASSERT_NE(selectionDescriptor, nullptr);
    EXPECT_EQ(selectionDescriptor->kind, PropertyEditorKind::command);
    EXPECT_EQ(
        choice_values(*selectionDescriptor),
        (std::vector<std::string>{"select", "add", "remove"}));

    const auto* scroll =
        descriptor_named(*snapshot.schema, "Scroll.Action");
    ASSERT_NE(scroll, nullptr);
    EXPECT_EQ(scroll->kind, PropertyEditorKind::command);
    EXPECT_EQ(
        choice_values(*scroll),
        (std::vector<std::string>{"up", "down"}));
}

TEST(XamlEnumParser, DeepCopiesAliasesAndSanitizesTypeNames) {
    std::vector<tap::EnumTypeInfo> copied;
    {
        auto* raw = static_cast<EnumType*>(
            CoTaskMemAlloc(sizeof(EnumType)));
        ASSERT_NE(raw, nullptr);
        ZeroMemory(raw, sizeof(EnumType));
        tap::OwnedEnumTypes owned(raw, 1);

        raw[0].Name =
            SysAllocString(L"Microsoft.UI.Xaml.Text\nAlignment");
        raw[0].ValueInts = SafeArrayCreateVector(VT_INT, 0, 3);
        raw[0].ValueStrings = SafeArrayCreateVector(VT_BSTR, 0, 3);
        ASSERT_NE(raw[0].Name, nullptr);
        ASSERT_NE(raw[0].ValueInts, nullptr);
        ASSERT_NE(raw[0].ValueStrings, nullptr);

        const int machineValues[] = {0, 0, 1};
        const wchar_t* names[] = {L"Left", L"Start", L"Center"};
        for (LONG index = 0; index < 3; ++index) {
            int machineValue = machineValues[index];
            ASSERT_EQ(
                SafeArrayPutElement(
                    raw[0].ValueInts, &index, &machineValue),
                S_OK);
            wil::unique_bstr name(SysAllocString(names[index]));
            ASSERT_TRUE(name);
            ASSERT_EQ(
                SafeArrayPutElement(
                    raw[0].ValueStrings, &index, name.get()),
                S_OK);
        }

        ASSERT_EQ(tap::copy_enum_types(
                      owned.get(), owned.count(), copied),
                  S_OK);
    }

    ASSERT_EQ(copied.size(), 1u);
    EXPECT_EQ(
        copied[0].name, L"Microsoft.UI.Xaml.TextAlignment");
    EXPECT_EQ(
        copied[0].flagsKind, XamlEnumFlagsKind::unknown);
    ASSERT_EQ(copied[0].members.size(), 3u);
    EXPECT_EQ(copied[0].members[0].machineValue, 0);
    EXPECT_EQ(copied[0].members[0].name, L"Left");
    EXPECT_EQ(copied[0].members[1].machineValue, 0);
    EXPECT_EQ(copied[0].members[1].name, L"Start");
    EXPECT_EQ(copied[0].members[2].machineValue, 1);
    EXPECT_EQ(copied[0].members[2].name, L"Center");
}

TEST(XamlEnumParser, MalformedCatalogStillReleasesOwnedMemory) {
    auto* raw = static_cast<EnumType*>(
        CoTaskMemAlloc(sizeof(EnumType)));
    ASSERT_NE(raw, nullptr);
    ZeroMemory(raw, sizeof(EnumType));
    tap::OwnedEnumTypes owned(raw, 1);
    raw[0].Name = SysAllocString(L"Broken.Enum");
    raw[0].ValueInts = SafeArrayCreateVector(VT_INT, 0, 1);
    ASSERT_NE(raw[0].Name, nullptr);
    ASSERT_NE(raw[0].ValueInts, nullptr);

    std::vector<tap::EnumTypeInfo> copied;
    EXPECT_EQ(
        tap::copy_enum_types(owned.get(), owned.count(), copied),
        E_INVALIDARG);
    EXPECT_TRUE(copied.empty());
}

TEST(XamlEnumCatalog, AssociatesChoicesCanonicalValuesAndAliases) {
    XamlEnumCatalog catalog;
    catalog.add(
        "Microsoft.UI.Xaml.TextAlignment",
        {
            {-1, "Unset"},
            {0, "Left"},
            {0, "Start"},
            {1, "Center"},
            {2, "Right"},
        },
        XamlEnumFlagsKind::nonFlags);

    const auto choices =
        catalog.choices_for("Microsoft.UI.Xaml.TextAlignment");
    ASSERT_EQ(choices.size(), 5u);
    EXPECT_EQ(choices[0].value, "Unset");
    EXPECT_EQ(choices[1].value, "Left");
    EXPECT_EQ(choices[2].value, "Start");
    EXPECT_EQ(choices[3].label, "Center");
    EXPECT_TRUE(catalog.accepts(
        "Microsoft.UI.Xaml.TextAlignment", "Center"));
    EXPECT_FALSE(catalog.accepts(
        "Microsoft.UI.Xaml.TextAlignment", "1"));
    EXPECT_EQ(
        catalog.canonical_value(
            "Microsoft.UI.Xaml.TextAlignment", "0"),
        std::optional<std::string>("Left"));
    EXPECT_EQ(
        catalog.canonical_value(
            "Microsoft.UI.Xaml.TextAlignment", "-1"),
        std::optional<std::string>("Unset"));
    EXPECT_EQ(
        catalog.canonical_value(
            "Microsoft.UI.Xaml.TextAlignment", "3"),
        std::optional<std::string>("3"));
    EXPECT_EQ(
        catalog.canonical_input(
            "Microsoft.UI.Xaml.TextAlignment", "Center,Right"),
        std::nullopt);
    EXPECT_EQ(
        catalog.canonical_value(
            "Microsoft.UI.Xaml.TextAlignment", "Center"),
        std::optional<std::string>("Center"));
}

TEST(XamlEnumCatalog, ConnectionCatalogsRemainIsolated) {
    XamlEnumCatalog systemXaml;
    XamlEnumCatalog winui;
    systemXaml.add(
        "Shared.Enum", {{0, "SystemValue"}},
        XamlEnumFlagsKind::nonFlags);
    winui.add(
        "Shared.Enum", {{0, "WinUIValue"}},
        XamlEnumFlagsKind::nonFlags);

    EXPECT_TRUE(systemXaml.accepts("Shared.Enum", "SystemValue"));
    EXPECT_FALSE(systemXaml.accepts("Shared.Enum", "WinUIValue"));
    EXPECT_TRUE(winui.accepts("Shared.Enum", "WinUIValue"));
    EXPECT_FALSE(winui.accepts("Shared.Enum", "SystemValue"));
}

TEST(XamlEnumCatalog, FlagsAcceptCompositeNamesAndPreserveResidualBits) {
    XamlEnumCatalog catalog;
    catalog.add(
        "Microsoft.UI.Xaml.Input.ManipulationModes",
        {
            {0, "None"},
            {1, "TranslateX"},
            {1, "TranslateHorizontal"},
            {2, "TranslateY"},
            {4, "Scale"},
        },
        XamlEnumFlagsKind::flags);

    EXPECT_EQ(
        catalog.canonical_input(
            "Microsoft.UI.Xaml.Input.ManipulationModes",
            " TranslateX , Scale "),
        std::optional<std::string>("TranslateX,Scale"));
    EXPECT_TRUE(catalog.accepts(
        "Microsoft.UI.Xaml.Input.ManipulationModes",
        "TranslateHorizontal,Scale"));
    EXPECT_EQ(
        catalog.canonical_input(
            "Microsoft.UI.Xaml.Input.ManipulationModes", " None "),
        std::optional<std::string>("None"));
    EXPECT_FALSE(catalog.accepts(
        "Microsoft.UI.Xaml.Input.ManipulationModes",
        "TranslateX,NotAFlag"));
    EXPECT_EQ(
        catalog.canonical_value(
            "Microsoft.UI.Xaml.Input.ManipulationModes", "5"),
        std::optional<std::string>("TranslateX,Scale"));
    EXPECT_EQ(
        catalog.canonical_value(
            "Microsoft.UI.Xaml.Input.ManipulationModes", "1"),
        std::optional<std::string>("TranslateX"));
    EXPECT_EQ(
        catalog.canonical_value(
            "Microsoft.UI.Xaml.Input.ManipulationModes", "0"),
        std::optional<std::string>("None"));
    EXPECT_EQ(
        catalog.canonical_value(
            "Microsoft.UI.Xaml.Input.ManipulationModes", "9"),
        std::optional<std::string>("TranslateX,0x00000008"));
}

TEST(XamlEnumCatalog, FlagsDetectionIsConservativeAndProviderOwned) {
    EXPECT_EQ(
        resolve_xaml_enum_flags_metadata(
            L"Windows.UI.Text.TextDecorations"),
        XamlEnumFlagsKind::flags);
    EXPECT_EQ(
        resolve_xaml_enum_flags_metadata(
            L"Windows.UI.Xaml.Input.ManipulationModes"),
        XamlEnumFlagsKind::flags);
    EXPECT_EQ(
        resolve_xaml_enum_flags_metadata(
            L"Windows.UI.Xaml.TextAlignment"),
        XamlEnumFlagsKind::nonFlags);
    EXPECT_EQ(
        resolve_xaml_enum_flags_metadata(
            L"Contoso.Unresolvable.CustomEnum"),
        XamlEnumFlagsKind::unknown);
}

TEST(XamlEnumCatalog, MetadataClassificationIsCachedPerType) {
    int probes = 0;
    XamlEnumFlagsCache cache(
        [&](std::wstring_view) {
            ++probes;
            return XamlEnumFlagsKind::flags;
        });

    EXPECT_EQ(
        cache.classify(L"Contoso.Flags"),
        XamlEnumFlagsKind::flags);
    EXPECT_EQ(
        cache.classify(L"Contoso.Flags"),
        XamlEnumFlagsKind::flags);
    EXPECT_EQ(probes, 1);
    EXPECT_EQ(cache.size(), 1u);
}

TEST(XamlEnumCatalog, UnresolvedTypesDeferCompositeInputWithoutDecomposition) {
    XamlEnumCatalog catalog;
    catalog.add(
        "Contoso.CustomEnum",
        {{1, "First"}, {2, "Second"}},
        XamlEnumFlagsKind::unknown);

    EXPECT_EQ(
        catalog.canonical_input(
            "Contoso.CustomEnum", " First , Second "),
        std::optional<std::string>("First,Second"));
    EXPECT_EQ(
        catalog.canonical_value("Contoso.CustomEnum", "3"),
        std::optional<std::string>("3"));
}

TEST(NativeTypedPropertyContract, ParsesOnlyCanonicalScalarShapes) {
    EXPECT_EQ(
        normalize_native_class_name("SysListView32"),
        "syslistview32");

    bool boolean = false;
    EXPECT_TRUE(native_property_detail::parse_boolean("true", boolean));
    EXPECT_TRUE(boolean);
    EXPECT_TRUE(native_property_detail::parse_boolean("FALSE", boolean));
    EXPECT_FALSE(boolean);
    EXPECT_FALSE(native_property_detail::parse_boolean("1", boolean));
    EXPECT_FALSE(native_property_detail::parse_boolean(" true", boolean));

    int integer = 0;
    EXPECT_TRUE(native_property_detail::parse_integer("-1", integer));
    EXPECT_EQ(integer, -1);
    EXPECT_TRUE(native_property_detail::parse_integer("2147483647", integer));
    EXPECT_EQ(integer, 2147483647);
    EXPECT_FALSE(native_property_detail::parse_integer("", integer));
    EXPECT_FALSE(native_property_detail::parse_integer("1.0", integer));
    EXPECT_FALSE(native_property_detail::parse_integer(" 1", integer));
    EXPECT_FALSE(native_property_detail::parse_integer("2147483648", integer));
}

TEST(NativeTypedPropertyContract, ComputesEffectiveScrollRange) {
    EXPECT_EQ(native_property_detail::effective_scroll_max(10, 110, 0), 110);
    EXPECT_EQ(native_property_detail::effective_scroll_max(10, 110, 1), 110);
    EXPECT_EQ(native_property_detail::effective_scroll_max(10, 110, 10), 101);
    EXPECT_EQ(native_property_detail::effective_scroll_max(10, 15, 20), 10);
    EXPECT_EQ(native_property_detail::effective_scroll_max(20, 10, 0), 20);
}

TEST(NativeTypedPropertyContract, PointerOperationsRequireMatchingArchitecture) {
    EXPECT_TRUE(native_pointer_operations_allowed(
        Architecture::x64, Architecture::x64));
    EXPECT_TRUE(native_pointer_operations_allowed(
        Architecture::x86, Architecture::x86));
    EXPECT_FALSE(native_pointer_operations_allowed(
        Architecture::x64, Architecture::x86));
    EXPECT_FALSE(native_pointer_operations_allowed(
        Architecture::x86, Architecture::x64));
    EXPECT_FALSE(native_pointer_operations_allowed(
        Architecture::unknown, Architecture::x64));
}

TEST(UiaTypedProperties, SchemaCacheIsBoundedUnderDynamicRangeChurn) {
    UiaPropertySchemaCache cache;
    std::string firstSchemaId;
    for (size_t i = 0;
         i < UiaPropertySchemaCache::kMaximumSchemas * 3;
         ++i) {
        auto slider = editable_uia_element(
            "Slider", "RangeValue",
            {{"RangeValue.Value", "1"},
             {"RangeValue.Minimum", std::to_string(i) + ".125"},
             {"RangeValue.Maximum", std::to_string(i + 1000) + ".875"},
             {"RangeValue.IsReadOnly", "false"}});
        auto snapshot = make_uia_property_snapshot(
            slider, UiaSelectionCapabilities{}, cache);
        if (i == 0)
            firstSchemaId = snapshot.schema->schemaId;
        EXPECT_LE(cache.size(), UiaPropertySchemaCache::kMaximumSchemas);
    }

    auto firstAgain = editable_uia_element(
        "Slider", "RangeValue",
        {{"RangeValue.Value", "1"},
         {"RangeValue.Minimum", "0.125"},
         {"RangeValue.Maximum", "1000.875"},
         {"RangeValue.IsReadOnly", "false"}});
    auto recreated = make_uia_property_snapshot(
        firstAgain, UiaSelectionCapabilities{}, cache);
    EXPECT_EQ(recreated.schema->schemaId, firstSchemaId);
    EXPECT_LE(cache.size(), UiaPropertySchemaCache::kMaximumSchemas);
}

TEST(XamlPropertyFilter, ArbitraryPropertiesWithUnrecognizedComplexTypesAreExcluded) {
    // A reference-typed property (a Brush, a Transform, another control) has
    // no meaningful flat string value to show, and its serialized value is
    // an opaque handle — excluded because its ValueType names an actual
    // class rather than one of the recognized primitive shapes.
    EXPECT_FALSE(lvt::xaml_should_capture_property(L"Foreground", L"123456789012", L"Brush"));
    EXPECT_FALSE(lvt::xaml_should_capture_property(L"RenderTransform", L"987654321098", L"TransformGroup"));
}

TEST(XamlPropertyFilter, LongDigitTextIsKeptWhenValueTypeIsConfirmedString) {
    // A phone number, order id, or timestamp in a real Text/Content property
    // must not be discarded just because it is long and all-digits: that
    // shape is indistinguishable from a XAML reference handle UNLESS
    // ValueType has already told us this is a real string.
    EXPECT_TRUE(lvt::xaml_should_capture_property(L"Text", L"5551234567890", L"String"));
    EXPECT_TRUE(lvt::xaml_should_capture_property(L"Content", L"20260821113000000", L"String"));
}

TEST(XamlPropertyFilter, LongDigitTextWithUnconfirmedTypeIsTreatedAsAHandle) {
    // Without a confirmed String ValueType, the shape heuristic is the only
    // signal available, so it still applies — this is the conservative
    // fallback, not a bug: it only ever suppresses text whose ValueType is
    // missing/unknown, never a value XAML has confirmed is a real string.
    EXPECT_FALSE(lvt::xaml_should_capture_property(L"Text", L"12345678901", L""));
    EXPECT_FALSE(lvt::xaml_should_capture_property(L"Content", L"12345678901", L"Object"));
}

TEST(XamlPropertyFilter, ShortDigitTextIsNeverTreatedAsAHandle) {
    // The >10-characters threshold exists so short numeric text (a 4-digit
    // PIN, a small order number) is never mistaken for a handle regardless
    // of ValueType.
    EXPECT_TRUE(lvt::xaml_should_capture_property(L"Text", L"1234567890", L""));
}

TEST(XamlPropertyFilter, NonNumericLongTextIsNeverTreatedAsAHandle) {
    EXPECT_TRUE(lvt::xaml_should_capture_property(
        L"Text", L"Order confirmation pending", L""));
}

TEST(XamlPropertyFilter, StatePropertiesBypassTheHandleHeuristicEntirely) {
    // AutomationProperties.AutomationId is always genuinely string-typed in
    // XAML, and a long numeric id (a generated GUID-as-string, say) is a very
    // plausible real value - it must not be mistaken for a handle just
    // because ValueType did not come through as a confirmed "String". State
    // properties skip the handle heuristic entirely, unlike text properties.
    EXPECT_TRUE(lvt::xaml_should_capture_property(
        L"AutomationProperties.AutomationId", L"123456789012", L""));
    EXPECT_TRUE(lvt::xaml_should_capture_property(L"Tag", L"123456789012", L"Object"));
    EXPECT_TRUE(lvt::xaml_should_capture_property(L"Source", L"123456789012", L""));
}

TEST(UiaCulture, FallsBackToTheNumberWhenUnresolvable) {
    EXPECT_EQ(lvt::uia_culture_name(0), "0");
    EXPECT_EQ(lvt::uia_culture_name(-1), "-1");
    EXPECT_EQ(lvt::uia_culture_name(0x7FFFFFF), "134217727");
}

TEST(FindElementByRef, NormalizesAndValidatesUiaReferences) {
    lvt::Element root;
    root.type = "Window";
    root.properties["RuntimeId"] = "42.100";
    lvt::Element child;
    child.type = "Button";
    child.properties["RuntimeId"] = "42.100.3.-7";
    root.children.push_back(child);
    lvt::assign_element_ids(root);

    // Negative components must resolve; the ref goes through the same
    // parse/format pair the walk uses to produce the property.
    auto* negative = lvt::find_element_by_ref(root, "uia:42.100.3.-7");
    ASSERT_NE(negative, nullptr);
    EXPECT_EQ(negative->type, "Button");

    // Leading zeros normalize to the emitted form rather than failing to match.
    auto* padded = lvt::find_element_by_ref(root, "uia:042.0100");
    ASSERT_NE(padded, nullptr);
    EXPECT_EQ(padded->type, "Window");

    // A malformed reference resolves to nothing rather than falling through to
    // the id/key lookups and matching something unrelated.
    EXPECT_EQ(lvt::find_element_by_ref(root, "uia:not-an-id"), nullptr);
    EXPECT_EQ(lvt::find_element_by_ref(root, "uia:"), nullptr);
}

// --- Key chord parsing ---
// press-key is the one interaction with no UIA pattern behind it, so the chord
// grammar is the whole contract.

TEST(KeyChord, ParsesBareKeys) {
    lvt::KeyChord chord;
    ASSERT_TRUE(lvt::parse_key_chord("Enter", chord));
    EXPECT_EQ(chord.vk, VK_RETURN);
    EXPECT_FALSE(chord.ctrl || chord.alt || chord.shift || chord.win);

    ASSERT_TRUE(lvt::parse_key_chord("escape", chord));
    EXPECT_EQ(chord.vk, VK_ESCAPE);
    ASSERT_TRUE(lvt::parse_key_chord("F5", chord));
    EXPECT_EQ(chord.vk, VK_F5);
    ASSERT_TRUE(lvt::parse_key_chord("f12", chord));
    EXPECT_EQ(chord.vk, VK_F12);
    ASSERT_TRUE(lvt::parse_key_chord("PageDown", chord));
    EXPECT_EQ(chord.vk, VK_NEXT);
}

TEST(KeyChord, ParsesModifiers) {
    lvt::KeyChord chord;
    ASSERT_TRUE(lvt::parse_key_chord("Ctrl+S", chord));
    EXPECT_TRUE(chord.ctrl);
    EXPECT_FALSE(chord.alt);
    EXPECT_NE(chord.vk, 0);
    // The case written by the caller must not become a Shift. Ctrl+S and
    // Ctrl+Shift+S are different shortcuts in most applications, so folding
    // VkKeyScan's case bit in here silently sends the wrong one.
    EXPECT_FALSE(chord.shift) << "Ctrl+S must not become Ctrl+Shift+S";

    ASSERT_TRUE(lvt::parse_key_chord("ctrl+shift+alt+F4", chord));
    EXPECT_TRUE(chord.ctrl);
    EXPECT_TRUE(chord.shift);
    EXPECT_TRUE(chord.alt);
    EXPECT_EQ(chord.vk, VK_F4);

    ASSERT_TRUE(lvt::parse_key_chord("Control+Home", chord));
    EXPECT_TRUE(chord.ctrl);
    EXPECT_EQ(chord.vk, VK_HOME);
}

TEST(SyntheticInput, PointIsOnScreenAcceptsTheDesktopAndRejectsOffscreenCoordinates) {
    // The guard that stops a click aimed at an offscreen element from being
    // clamped onto whatever sits at the desktop's corner. Offscreen XAML
    // elements routinely report coordinates like -20237,-21283.
    POINT onScreen{GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2};
    EXPECT_TRUE(lvt::point_is_on_screen(onScreen));

    EXPECT_FALSE(lvt::point_is_on_screen(POINT{-20237, -21283}));
    EXPECT_FALSE(lvt::point_is_on_screen(POINT{1'000'000, 1'000'000}));
}

TEST(SyntheticInput, PointIsOnScreenAcceptsTheVirtualDesktopEdges) {
    // Multi-monitor coordinates can legitimately be negative, so the check must
    // be against the virtual desktop rather than the primary monitor.
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    EXPECT_TRUE(lvt::point_is_on_screen(POINT{left, top}));
    EXPECT_TRUE(lvt::point_is_on_screen(POINT{left + width - 1, top + height - 1}));
    // Just outside the virtual desktop must be rejected.
    EXPECT_FALSE(lvt::point_is_on_screen(POINT{left + width + 500, top + height + 500}));
}

TEST(KeyChord, UppercaseLettersWithModifiersDoNotGainAPhantomShift) {    // Every one of these is a real, commonly used shortcut whose Shift variant
    // does something different, which is what makes the bug this pins down
    // more than cosmetic.
    for (const char* text : {"Ctrl+S", "Ctrl+A", "Ctrl+N", "Ctrl+O", "Ctrl+W", "Alt+F"}) {
        lvt::KeyChord chord;
        ASSERT_TRUE(lvt::parse_key_chord(text, chord)) << text;
        EXPECT_FALSE(chord.shift) << text << " must not imply Shift";
    }

    // The lowercase spelling has always been correct, and must stay identical
    // to the uppercase one now that both are accepted.
    lvt::KeyChord upper;
    lvt::KeyChord lower;
    ASSERT_TRUE(lvt::parse_key_chord("Ctrl+N", upper));
    ASSERT_TRUE(lvt::parse_key_chord("Ctrl+n", lower));
    EXPECT_EQ(upper.vk, lower.vk);
    EXPECT_EQ(upper.shift, lower.shift);
    EXPECT_EQ(upper.ctrl, lower.ctrl);
}

TEST(KeyChord, ExplicitShiftIsStillHonouredWithLetters) {
    lvt::KeyChord chord;
    ASSERT_TRUE(lvt::parse_key_chord("Ctrl+Shift+S", chord));
    EXPECT_TRUE(chord.ctrl);
    EXPECT_TRUE(chord.shift);

    // Shift written on its own with a lowercase letter must still shift.
    ASSERT_TRUE(lvt::parse_key_chord("Shift+a", chord));
    EXPECT_TRUE(chord.shift);
}

TEST(KeyChord, PunctuationKeepsTheLayoutShiftEvenWithModifiers) {
    // Unlike a letter's case, the Shift on punctuation is how the character is
    // produced at all: dropping it would send Ctrl+/ instead of Ctrl+?. This is
    // the distinction that stops the fix for the letter case from overreaching.
    //
    // Guarded on the layout actually needing Shift for '?', so the test does
    // not fail on layouts where it does not.
    const SHORT scan = VkKeyScanW(L'?');
    if (scan == -1 || !((scan >> 8) & 1))
        GTEST_SKIP() << "this keyboard layout does not reach '?' via Shift";

    lvt::KeyChord chord;
    ASSERT_TRUE(lvt::parse_key_chord("Ctrl+?", chord));
    EXPECT_TRUE(chord.ctrl);
    EXPECT_TRUE(chord.shift) << "punctuation must keep the shift that produces it";
}

TEST(KeyChord, ShiftedCharactersCarryTheirOwnModifier) {
    // VkKeyScan reports the shift state a character needs, so an uppercase
    // letter must not silently type a lowercase one.
    lvt::KeyChord upper;
    ASSERT_TRUE(lvt::parse_key_chord("A", upper));
    lvt::KeyChord lower;
    ASSERT_TRUE(lvt::parse_key_chord("a", lower));
    EXPECT_EQ(upper.vk, lower.vk);
    EXPECT_TRUE(upper.shift);
    EXPECT_FALSE(lower.shift);
}

TEST(KeyChord, HandlesPlusAsAKey) {
    // "Ctrl++" is a real accelerator (zoom in), and naive splitting on '+'
    // turns it into a trailing empty token.
    lvt::KeyChord chord;
    ASSERT_TRUE(lvt::parse_key_chord("Ctrl++", chord));
    EXPECT_TRUE(chord.ctrl);
    EXPECT_NE(chord.vk, 0);
}

TEST(KeyChord, RejectsMalformedInput) {
    lvt::KeyChord chord;
    EXPECT_FALSE(lvt::parse_key_chord("", chord));
    EXPECT_FALSE(lvt::parse_key_chord("   ", chord));
    EXPECT_FALSE(lvt::parse_key_chord("Ctrl+", chord));
    EXPECT_FALSE(lvt::parse_key_chord("Ctrl", chord));      // modifier with no key
    EXPECT_FALSE(lvt::parse_key_chord("NotAKey", chord));
    EXPECT_FALSE(lvt::parse_key_chord("F0", chord));
    EXPECT_FALSE(lvt::parse_key_chord("F25", chord));
    EXPECT_FALSE(lvt::parse_key_chord("Bogus+A", chord));   // unknown modifier
}

TEST(KeyChord, ParsesSequences) {
    std::vector<lvt::KeyChord> chords;
    ASSERT_TRUE(lvt::parse_key_chords("Ctrl+A;Delete", chords));
    ASSERT_EQ(chords.size(), 2u);
    EXPECT_TRUE(chords[0].ctrl);
    EXPECT_EQ(chords[1].vk, VK_DELETE);

    ASSERT_TRUE(lvt::parse_key_chords("Enter, Tab , Escape", chords));
    ASSERT_EQ(chords.size(), 3u);
    EXPECT_EQ(chords[0].vk, VK_RETURN);
    EXPECT_EQ(chords[1].vk, VK_TAB);
    EXPECT_EQ(chords[2].vk, VK_ESCAPE);

    // One bad chord fails the whole sequence rather than silently sending part.
    EXPECT_FALSE(lvt::parse_key_chords("Enter;NotAKey", chords));
    EXPECT_FALSE(lvt::parse_key_chords("", chords));
}

TEST(ActionKind, ParsesAndRoundTripsNames) {
    lvt::ActionKind kind;
    ASSERT_TRUE(lvt::parse_action_kind("click", kind));
    EXPECT_EQ(kind, lvt::ActionKind::click);
    ASSERT_TRUE(lvt::parse_action_kind("set-value", kind));
    EXPECT_EQ(kind, lvt::ActionKind::setValue);
    ASSERT_TRUE(lvt::parse_action_kind("SetValue", kind));
    EXPECT_EQ(kind, lvt::ActionKind::setValue);
    EXPECT_FALSE(lvt::parse_action_kind("nonsense", kind));

    EXPECT_STREQ(lvt::action_kind_name(lvt::ActionKind::click), "click");
    EXPECT_STREQ(lvt::action_kind_name(lvt::ActionKind::setValue), "set-value");
}

// Every ActionKind, so a new one cannot be added without a name. The previous
// spot-check of two kinds let nine of them silently report themselves as
// "unknown" in the result JSON.
static constexpr lvt::ActionKind kAllActionKinds[] = {
    lvt::ActionKind::click,          lvt::ActionKind::invoke,
    lvt::ActionKind::toggle,         lvt::ActionKind::setValue,
    lvt::ActionKind::expand,         lvt::ActionKind::collapse,
    lvt::ActionKind::select,         lvt::ActionKind::addToSelection,
    lvt::ActionKind::removeFromSelection, lvt::ActionKind::selectText,
    lvt::ActionKind::focus,          lvt::ActionKind::scroll,
    lvt::ActionKind::typeText,       lvt::ActionKind::pressKey,
    lvt::ActionKind::windowClose,    lvt::ActionKind::windowMinimize,
    lvt::ActionKind::windowMaximize, lvt::ActionKind::windowRestore,
    lvt::ActionKind::waitFor,        lvt::ActionKind::waitGone,
};

TEST(ActionKind, EveryKindHasARealName) {
    for (const auto kind : kAllActionKinds) {
        const std::string name = lvt::action_kind_name(kind);
        EXPECT_NE(name, "unknown")
            << "ActionKind " << static_cast<int>(kind)
            << " has no name, so it reports itself as \"unknown\" in the result JSON";
        EXPECT_FALSE(name.empty());
    }
}

TEST(ActionKind, EveryNameParsesBackToItsOwnKind) {
    for (const auto kind : kAllActionKinds) {
        const auto* name = lvt::action_kind_name(kind);
        lvt::ActionKind parsed{};
        ASSERT_TRUE(lvt::parse_action_kind(name, parsed))
            << "'" << name << "' is emitted but not accepted back";
        EXPECT_EQ(parsed, kind) << "'" << name << "' round-tripped to a different kind";
    }
}

TEST(ActionKind, NamesAreUnique) {
    // Two kinds sharing a name would make the round trip above pass while the
    // reported action was still wrong for one of them.
    std::set<std::string> seen;
    for (const auto kind : kAllActionKinds) {
        const std::string name = lvt::action_kind_name(kind);
        EXPECT_TRUE(seen.insert(name).second) << "duplicate action name '" << name << "'";
    }
    EXPECT_EQ(seen.size(), std::size(kAllActionKinds));
}

TEST(ActionKind, CliVerbNamesMatchTheReportedActionNames) {
    // The verb a user types and the action lvt reports having performed should
    // be the same word; a mismatch makes result JSON hard to correlate with the
    // command that produced it.
    const std::pair<const char*, lvt::ActionKind> pairs[] = {
        {"click", lvt::ActionKind::click},
        {"invoke", lvt::ActionKind::invoke},
        {"toggle", lvt::ActionKind::toggle},
        {"set-value", lvt::ActionKind::setValue},
        {"expand", lvt::ActionKind::expand},
        {"collapse", lvt::ActionKind::collapse},
        {"select", lvt::ActionKind::select},
        {"add-to-selection", lvt::ActionKind::addToSelection},
        {"remove-from-selection", lvt::ActionKind::removeFromSelection},
        {"select-text", lvt::ActionKind::selectText},
        {"focus", lvt::ActionKind::focus},
        {"scroll", lvt::ActionKind::scroll},
        {"type", lvt::ActionKind::typeText},
        {"press-key", lvt::ActionKind::pressKey},
        {"close", lvt::ActionKind::windowClose},
        {"minimize", lvt::ActionKind::windowMinimize},
        {"maximize", lvt::ActionKind::windowMaximize},
        {"restore", lvt::ActionKind::windowRestore},
        {"wait-for", lvt::ActionKind::waitFor},
        {"wait-gone", lvt::ActionKind::waitGone},
    };
    for (const auto& [verb, kind] : pairs)
        EXPECT_STREQ(lvt::action_kind_name(kind), verb);
}
