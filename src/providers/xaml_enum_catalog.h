#pragma once

#include "framework_connection.h"
#include "../xaml_enum_util.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lvt {

struct XamlEnumMember {
    int32_t machineValue = 0;
    std::string name;
};

struct XamlEnumTypeInfo {
    bool isFlags = false;
    std::vector<XamlEnumMember> members;
};

class XamlEnumCatalog {
public:
    void add(
        std::string typeName, std::vector<XamlEnumMember> members,
        bool isFlags = false) {
        m_types.insert_or_assign(
            std::move(typeName),
            XamlEnumTypeInfo{isFlags, std::move(members)});
    }

    const XamlEnumTypeInfo* find(
        const std::string& typeName) const {
        const auto found = m_types.find(typeName);
        return found == m_types.end() ? nullptr : &found->second;
    }

    std::vector<PropertyChoice> choices_for(
        const std::string& typeName) const {
        std::vector<PropertyChoice> choices;
        const auto* type = find(typeName);
        if (!type)
            return choices;
        choices.reserve(type->members.size());
        for (const auto& member : type->members)
            choices.push_back({member.name, member.name});
        return choices;
    }

    bool accepts(
        const std::string& typeName, const std::string& value) const {
        return canonical_input(typeName, value).has_value();
    }

    std::optional<std::string> canonical_input(
        const std::string& typeName, const std::string& value) const {
        const auto* type = find(typeName);
        if (!type)
            return std::nullopt;
        return detail::canonicalize_enum_member_list(
            value, type->members, type->isFlags);
    }

    std::optional<std::string> canonical_value(
        const std::string& typeName, const std::string& value) const {
        const auto* type = find(typeName);
        if (!type)
            return std::nullopt;
        if (auto names =
                detail::canonicalize_enum_member_list(
                    value, type->members, type->isFlags)) {
            return names;
        }

        int32_t numeric = 0;
        const auto parsed = std::from_chars(
            value.data(), value.data() + value.size(), numeric, 10);
        if (parsed.ec != std::errc() ||
            parsed.ptr != value.data() + value.size()) {
            return std::nullopt;
        }
        for (const auto& member : type->members) {
            if (member.machineValue == numeric)
                return member.name;
        }
        if (!type->isFlags)
            return value;

        uint32_t remaining = static_cast<uint32_t>(numeric);
        std::string composite;
        std::vector<uint32_t> seenValues;
        for (const auto& member : type->members) {
            const auto bits = static_cast<uint32_t>(member.machineValue);
            if (bits == 0 ||
                std::find(
                    seenValues.begin(), seenValues.end(), bits) !=
                    seenValues.end()) {
                continue;
            }
            seenValues.push_back(bits);
            if ((remaining & bits) != bits)
                continue;
            if (!composite.empty())
                composite += ",";
            composite += member.name;
            remaining &= ~bits;
        }
        if (remaining != 0) {
            char residual[16];
            snprintf(residual, sizeof(residual), "0x%08X", remaining);
            if (!composite.empty())
                composite += ",";
            composite += residual;
        }
        if (!composite.empty())
            return composite;

        // A zero value without a named zero/None member still needs to remain
        // explicit; inventing a member name would misrepresent the runtime.
        return std::string("0");
    }

    size_t size() const noexcept { return m_types.size(); }

private:
    std::unordered_map<std::string, XamlEnumTypeInfo> m_types;
};

} // namespace lvt
