#include "xaml_flags_metadata.h"

#include <Windows.h>
#include <rometadataresolution.h>
#include <wil/com.h>
#include <wil/resource.h>

namespace lvt {

const char* xaml_enum_flags_kind_name(XamlEnumFlagsKind kind) {
    switch (kind) {
    case XamlEnumFlagsKind::nonFlags: return "no";
    case XamlEnumFlagsKind::flags: return "yes";
    case XamlEnumFlagsKind::unknown: return "unknown";
    }
    return "unknown";
}

XamlEnumFlagsKind parse_xaml_enum_flags_kind(std::string_view value) {
    if (value == "no")
        return XamlEnumFlagsKind::nonFlags;
    if (value == "yes")
        return XamlEnumFlagsKind::flags;
    return XamlEnumFlagsKind::unknown;
}

XamlEnumFlagsKind resolve_xaml_enum_flags_metadata(
    std::wstring_view typeName) {
    if (typeName.empty())
        return XamlEnumFlagsKind::unknown;

    wil::unique_hstring runtimeType;
    if (FAILED(WindowsCreateString(
            typeName.data(), static_cast<UINT32>(typeName.size()),
            runtimeType.put()))) {
        return XamlEnumFlagsKind::unknown;
    }

    wil::com_ptr<IMetaDataImport2> metadata;
    mdTypeDef typeToken = mdTypeDefNil;
    if (FAILED(RoGetMetaDataFile(
            runtimeType.get(), nullptr, nullptr, metadata.put(),
            &typeToken)) ||
        !metadata || typeToken == mdTypeDefNil) {
        return XamlEnumFlagsKind::unknown;
    }

    const void* attributeData = nullptr;
    ULONG attributeSize = 0;
    const HRESULT attributeHr = metadata->GetCustomAttributeByName(
        typeToken, L"System.FlagsAttribute",
        &attributeData, &attributeSize);
    if (attributeHr == S_OK)
        return XamlEnumFlagsKind::flags;
    if (attributeHr == S_FALSE ||
        attributeHr == CLDB_E_RECORD_NOTFOUND) {
        return XamlEnumFlagsKind::nonFlags;
    }
    return XamlEnumFlagsKind::unknown;
}

} // namespace lvt
