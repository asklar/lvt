#pragma once

#include "framework_connection.h"

#include <charconv>
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

class XamlEnumCatalog {
public:
    void add(std::string typeName, std::vector<XamlEnumMember> members) {
        m_types.insert_or_assign(std::move(typeName), std::move(members));
    }

    const std::vector<XamlEnumMember>* find(
        const std::string& typeName) const {
        const auto found = m_types.find(typeName);
        return found == m_types.end() ? nullptr : &found->second;
    }

    std::vector<PropertyChoice> choices_for(
        const std::string& typeName) const {
        std::vector<PropertyChoice> choices;
        const auto* members = find(typeName);
        if (!members)
            return choices;
        choices.reserve(members->size());
        for (const auto& member : *members)
            choices.push_back({member.name, member.name});
        return choices;
    }

    bool accepts(
        const std::string& typeName, const std::string& value) const {
        const auto* members = find(typeName);
        if (!members)
            return false;
        for (const auto& member : *members) {
            if (member.name == value)
                return true;
        }
        return false;
    }

    std::optional<std::string> canonical_value(
        const std::string& typeName, const std::string& value) const {
        const auto* members = find(typeName);
        if (!members)
            return std::nullopt;
        for (const auto& member : *members) {
            if (member.name == value)
                return member.name;
        }

        int32_t numeric = 0;
        const auto parsed = std::from_chars(
            value.data(), value.data() + value.size(), numeric, 10);
        if (parsed.ec != std::errc() ||
            parsed.ptr != value.data() + value.size()) {
            return std::nullopt;
        }
        for (const auto& member : *members) {
            if (member.machineValue == numeric)
                return member.name;
        }
        return std::nullopt;
    }

    size_t size() const noexcept { return m_types.size(); }

private:
    std::unordered_map<std::string, std::vector<XamlEnumMember>> m_types;
};

} // namespace lvt
