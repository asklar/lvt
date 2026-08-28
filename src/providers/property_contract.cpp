#include "framework_connection.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace lvt {
namespace {

std::string simple_type_name(std::string_view type) {
    const auto separator = type.find_last_of(".:");
    std::string name(type.substr(
        separator == std::string_view::npos ? 0 : separator + 1));
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return name;
}

} // namespace

const char* property_editor_kind_name(PropertyEditorKind kind) {
    switch (kind) {
    case PropertyEditorKind::readonly: return "readonly";
    case PropertyEditorKind::string: return "string";
    case PropertyEditorKind::boolean: return "boolean";
    case PropertyEditorKind::integer: return "integer";
    case PropertyEditorKind::number: return "number";
    case PropertyEditorKind::enumeration: return "enum";
    case PropertyEditorKind::command: return "command";
    }
    return "readonly";
}

PropertyEditorKind classify_property_editor(
    std::string_view declaredType, bool writable) {
    if (!writable || declaredType.empty())
        return PropertyEditorKind::readonly;

    const auto type = simple_type_name(declaredType);
    if (type == "boolean" || type == "bool")
        return PropertyEditorKind::boolean;
    if (type == "byte" || type == "sbyte" ||
        type == "int16" || type == "uint16" ||
        type == "int32" || type == "uint32" ||
        type == "int64" || type == "uint64" ||
        type == "integer") {
        return PropertyEditorKind::integer;
    }
    if (type == "single" || type == "double" ||
        type == "decimal" || type == "float" ||
        type == "number") {
        return PropertyEditorKind::number;
    }
    if (type == "enum" || type == "enumeration")
        return PropertyEditorKind::enumeration;

    // xamlOM can report custom scalar type names. The provider still owns
    // conversion, so a plain text editor preserves that existing capability
    // without teaching the Viewer framework-specific type catalogs.
    return PropertyEditorKind::string;
}

} // namespace lvt
