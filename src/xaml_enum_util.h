#pragma once

#include <algorithm>
#include <optional>
#include <string>

namespace lvt::detail {

template <typename String, typename Members>
std::optional<String> canonicalize_enum_member_list(
    const String& value, const Members& members, bool allowComposite) {
    using Char = typename String::value_type;
    const auto is_space = [](Char ch) {
        return ch == static_cast<Char>(' ') ||
               ch == static_cast<Char>('\t') ||
               ch == static_cast<Char>('\r') ||
               ch == static_cast<Char>('\n') ||
               ch == static_cast<Char>('\f') ||
               ch == static_cast<Char>('\v');
    };

    String canonical;
    size_t start = 0;
    for (;;) {
        const auto comma = value.find(static_cast<Char>(','), start);
        if (comma != String::npos && !allowComposite)
            return std::nullopt;
        const auto end = comma == String::npos ? value.size() : comma;
        size_t first = start;
        while (first < end && is_space(value[first]))
            ++first;
        size_t last = end;
        while (last > first && is_space(value[last - 1]))
            --last;
        if (first == last)
            return std::nullopt;

        const String token = value.substr(first, last - first);
        const auto member = std::find_if(
            members.begin(), members.end(),
            [&](const auto& candidate) {
                return candidate.name == token;
            });
        if (member == members.end())
            return std::nullopt;
        if (!canonical.empty())
            canonical.push_back(static_cast<Char>(','));
        canonical += member->name;

        if (comma == String::npos)
            break;
        start = comma + 1;
    }
    return canonical;
}

} // namespace lvt::detail
