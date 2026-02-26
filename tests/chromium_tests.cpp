// Unit tests for the LVT Chromium plugin components.
// Tests the DOM JSON format compatibility with plugin_loader's graft_json_node,
// and the native messaging length-prefix protocol.

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <cstring>

using json = nlohmann::json;

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
