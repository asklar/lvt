// Unit tests for LVT — pure logic, no live windows required

#include <gtest/gtest.h>
#include "element.h"
#include "tree_builder.h"
#include "json_serializer.h"
#include "framework_detector.h"
#include "target.h"
#include <nlohmann/json.hpp>
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
    EXPECT_TRUE(el.type.empty());
    EXPECT_TRUE(el.framework.empty());
    EXPECT_TRUE(el.className.empty());
    EXPECT_TRUE(el.text.empty());
    EXPECT_TRUE(el.properties.empty());
    EXPECT_TRUE(el.children.empty());
    EXPECT_EQ(el.nativeHandle, 0u);
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
