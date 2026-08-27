#pragma once

#include "providers/framework_connection.h"

#include <cstdint>
#include <string>

namespace lvt {

enum class WatchControlCommand {
    getProperties,
    setProperty,
    clearProperty,
};

struct XamlElementKey {
    std::string framework;
    uintptr_t handle = 0;
};

struct WatchControlRequest {
    uint64_t requestId = 0;
    WatchControlCommand command = WatchControlCommand::getProperties;
    XamlElementKey element;
    uint32_t propertyIndex = 0;
    std::string valueType;
    std::string value;
};

struct WatchControlParseResult {
    bool ok = false;
    uint64_t requestId = 0;
    WatchControlRequest request;
    HRESULT hresult = E_INVALIDARG;
    std::string error;
};

// Parses only compact XAML diagnostics identities: xaml:0xHANDLE and
// winui3:0xHANDLE. Structural keys deliberately do not resolve to an object
// handle and therefore cannot be used for native property operations.
bool parse_xaml_element_key(const std::string& text, XamlElementKey& out,
                            std::string& error);

// Parses one external watch --control NDJSON request. Any requestId that can
// be recovered is preserved in an error result so the caller can correlate
// malformed requests as well as successful ones.
WatchControlParseResult parse_watch_control_request(const std::string& line);

std::string serialize_watch_command_result(
    uint64_t requestId, const FrameworkPropertyResult& result);
std::string serialize_watch_command_error(
    uint64_t requestId, HRESULT hresult, const std::string& error);

} // namespace lvt
