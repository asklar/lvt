#include "watch_control.h"

#include <charconv>
#include <cstdio>
#include <limits>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace lvt {

bool parse_xaml_element_key(const std::string& text, XamlElementKey& out,
                            std::string& error) {
    std::string framework;
    size_t digitsStart = 0;
    if (text.rfind("xaml:0x", 0) == 0) {
        framework = "xaml";
        digitsStart = 7;
    } else if (text.rfind("winui3:0x", 0) == 0) {
        framework = "winui3";
        digitsStart = 9;
    } else {
        error = "Only compact XAML/WinUI3 keys (xaml:0xHANDLE or "
                "winui3:0xHANDLE) support property commands";
        return false;
    }

    const char* first = text.data() + digitsStart;
    const char* last = text.data() + text.size();
    if (first == last) {
        error = "XAML element key is missing its hexadecimal instance handle";
        return false;
    }

    uintptr_t handle = 0;
    auto parsed = std::from_chars(first, last, handle, 16);
    if (parsed.ec != std::errc() || parsed.ptr != last || handle == 0) {
        error = "XAML element key has an invalid hexadecimal instance handle";
        return false;
    }

    out.framework = std::move(framework);
    out.handle = handle;
    return true;
}

static bool read_request_id(const json& input, uint64_t& requestId) {
    auto it = input.find("requestId");
    if (it == input.end())
        return false;
    if (it->is_number_unsigned()) {
        requestId = it->get<uint64_t>();
        return true;
    }
    if (it->is_number_integer()) {
        auto signedId = it->get<int64_t>();
        if (signedId >= 0) {
            requestId = static_cast<uint64_t>(signedId);
            return true;
        }
    }
    return false;
}

WatchControlParseResult parse_watch_control_request(const std::string& line) {
    WatchControlParseResult result;
    json input = json::parse(line, nullptr, false);
    if (input.is_discarded() || !input.is_object()) {
        result.error = "Control request must be one valid JSON object";
        return result;
    }

    if (!read_request_id(input, result.requestId)) {
        result.error = "Control request requires a non-negative integer requestId";
        return result;
    }
    result.request.requestId = result.requestId;

    auto commandIt = input.find("command");
    if (commandIt == input.end() || !commandIt->is_string()) {
        result.error = "Control request requires a string command";
        return result;
    }
    const std::string command = commandIt->get<std::string>();
    if (command == "get_properties") {
        result.request.command = WatchControlCommand::getProperties;
    } else if (command == "set_property") {
        result.request.command = WatchControlCommand::setProperty;
    } else if (command == "clear_property") {
        result.request.command = WatchControlCommand::clearProperty;
    } else {
        result.error = "Unknown control command '" + command + "'";
        return result;
    }

    auto keyIt = input.find("key");
    if (keyIt == input.end() || !keyIt->is_string()) {
        result.error = "Control request requires a string key";
        return result;
    }
    if (!parse_xaml_element_key(
            keyIt->get_ref<const std::string&>(), result.request.element, result.error)) {
        return result;
    }

    if (result.request.command != WatchControlCommand::getProperties) {
        auto indexIt = input.find("propertyIndex");
        uint64_t index = 0;
        if (indexIt == input.end() ||
            (!indexIt->is_number_unsigned() && !indexIt->is_number_integer())) {
            result.error = "Property command requires a non-negative integer propertyIndex";
            return result;
        }
        if (indexIt->is_number_integer()) {
            auto signedIndex = indexIt->get<int64_t>();
            if (signedIndex < 0) {
                result.error = "propertyIndex must be a non-negative 32-bit integer";
                return result;
            }
            index = static_cast<uint64_t>(signedIndex);
        } else {
            index = indexIt->get<uint64_t>();
        }
        if (index > std::numeric_limits<uint32_t>::max()) {
            result.error = "propertyIndex must be a non-negative 32-bit integer";
            return result;
        }
        result.request.propertyIndex = static_cast<uint32_t>(index);
    }

    if (result.request.command == WatchControlCommand::setProperty) {
        auto typeIt = input.find("valueType");
        auto valueIt = input.find("value");
        if (typeIt == input.end() || !typeIt->is_string() ||
            typeIt->get_ref<const std::string&>().empty()) {
            result.error = "set_property requires a non-empty string valueType";
            return result;
        }
        if (valueIt == input.end() || !valueIt->is_string()) {
            result.error = "set_property requires a string value";
            return result;
        }
        result.request.valueType = typeIt->get<std::string>();
        result.request.value = valueIt->get<std::string>();
    }

    result.ok = true;
    result.hresult = S_OK;
    return result;
}

static std::string format_hresult(HRESULT hresult) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "0x%08lX",
             static_cast<unsigned long>(hresult));
    return buffer;
}

std::string serialize_watch_command_result(
    uint64_t requestId, const FrameworkPropertyResult& result) {
    json output{
        {"event", "command_result"},
        {"requestId", requestId},
        {"ok", result.ok},
    };
    if (!result.ok) {
        output["error"] = result.error.empty() ? "Property command failed" : result.error;
        output["hresult"] = format_hresult(result.hresult);
    } else {
        if (result.hasProperties) {
            output["properties"] = json::array();
            for (const auto& property : result.properties) {
                output["properties"].push_back({
                    {"name", property.name},
                    {"value", property.value},
                    {"valueType", property.valueType},
                    {"declaringType", property.declaringType},
                    {"propertyIndex", property.propertyIndex},
                    {"metadataBits", property.metadataBits},
                    {"overridden", property.overridden},
                    {"source", property.source},
                });
            }
        }
        if (result.hasValue)
            output["value"] = result.value;
    }
    return output.dump();
}

std::string serialize_watch_command_error(
    uint64_t requestId, HRESULT hresult, const std::string& error) {
    FrameworkPropertyResult result;
    result.hresult = hresult;
    result.error = error;
    return serialize_watch_command_result(requestId, result);
}

} // namespace lvt
