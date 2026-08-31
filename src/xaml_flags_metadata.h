#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace lvt {

enum class XamlEnumFlagsKind {
    nonFlags,
    flags,
    unknown,
};

const char* xaml_enum_flags_kind_name(XamlEnumFlagsKind kind);
XamlEnumFlagsKind parse_xaml_enum_flags_kind(std::string_view value);
XamlEnumFlagsKind resolve_xaml_enum_flags_metadata(
    std::wstring_view typeName);

class XamlEnumFlagsCache {
public:
    using Resolver =
        std::function<XamlEnumFlagsKind(std::wstring_view)>;

    explicit XamlEnumFlagsCache(
        Resolver resolver = resolve_xaml_enum_flags_metadata)
        : m_resolver(std::move(resolver)) {}

    XamlEnumFlagsKind classify(const std::wstring& typeName) {
        const auto found = m_values.find(typeName);
        if (found != m_values.end())
            return found->second;
        const auto kind = m_resolver(typeName);
        m_values.emplace(typeName, kind);
        return kind;
    }

    size_t size() const noexcept { return m_values.size(); }

private:
    Resolver m_resolver;
    std::unordered_map<std::wstring, XamlEnumFlagsKind> m_values;
};

} // namespace lvt
