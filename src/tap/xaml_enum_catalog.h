#pragma once

#include <Windows.h>
#include <xamlOM.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "../xaml_enum_util.h"

namespace lvt::tap {

struct EnumMember {
    int32_t machineValue = 0;
    std::wstring name;
};

struct EnumTypeInfo {
    std::wstring name;
    bool isFlags = false;
    std::vector<EnumMember> members;
};

inline std::wstring sanitize_enum_type_name(const std::wstring& value) {
    std::wstring sanitized;
    sanitized.reserve(value.size());
    for (wchar_t ch : value) {
        if (ch >= 0x20)
            sanitized.push_back(ch);
    }
    return sanitized;
}

inline void destroy_enum_types(EnumType* values, unsigned int count) noexcept {
    if (!values)
        return;
    for (unsigned int i = 0; i < count; ++i) {
        SysFreeString(values[i].Name);
        if (values[i].ValueInts)
            SafeArrayDestroy(values[i].ValueInts);
        if (values[i].ValueStrings)
            SafeArrayDestroy(values[i].ValueStrings);
    }
    CoTaskMemFree(values);
}

class OwnedEnumTypes {
public:
    OwnedEnumTypes(EnumType* values, unsigned int count) noexcept
        : m_values(values), m_count(count) {}
    ~OwnedEnumTypes() { destroy_enum_types(m_values, m_count); }

    OwnedEnumTypes(const OwnedEnumTypes&) = delete;
    OwnedEnumTypes& operator=(const OwnedEnumTypes&) = delete;

    EnumType* get() const noexcept { return m_values; }
    unsigned int count() const noexcept { return m_count; }

private:
    EnumType* m_values = nullptr;
    unsigned int m_count = 0;
};

inline HRESULT copy_enum_types(
    const EnumType* values, unsigned int count,
    std::vector<EnumTypeInfo>& output) {
    output.clear();
    if (count != 0 && !values)
        return E_INVALIDARG;

    try {
        std::vector<EnumTypeInfo> parsedCatalog;
        parsedCatalog.reserve(count);
        for (unsigned int i = 0; i < count; ++i) {
            if (!values[i].Name || !values[i].ValueInts ||
                !values[i].ValueStrings) {
                return E_INVALIDARG;
            }
            if (SafeArrayGetDim(values[i].ValueInts) != 1 ||
                SafeArrayGetDim(values[i].ValueStrings) != 1) {
                return E_INVALIDARG;
            }
            VARTYPE intsType = VT_EMPTY;
            VARTYPE stringsType = VT_EMPTY;
            RETURN_IF_FAILED(SafeArrayGetVartype(
                values[i].ValueInts, &intsType));
            RETURN_IF_FAILED(SafeArrayGetVartype(
                values[i].ValueStrings, &stringsType));
            if ((intsType != VT_INT && intsType != VT_I4) ||
                stringsType != VT_BSTR) {
                return DISP_E_TYPEMISMATCH;
            }

            LONG intsLower = 0;
            LONG intsUpper = -1;
            LONG stringsLower = 0;
            LONG stringsUpper = -1;
            RETURN_IF_FAILED(SafeArrayGetLBound(
                values[i].ValueInts, 1, &intsLower));
            RETURN_IF_FAILED(SafeArrayGetUBound(
                values[i].ValueInts, 1, &intsUpper));
            RETURN_IF_FAILED(SafeArrayGetLBound(
                values[i].ValueStrings, 1, &stringsLower));
            RETURN_IF_FAILED(SafeArrayGetUBound(
                values[i].ValueStrings, 1, &stringsUpper));
            if (intsUpper - intsLower != stringsUpper - stringsLower)
                return E_INVALIDARG;

            EnumTypeInfo copied;
            copied.name = sanitize_enum_type_name(std::wstring(
                values[i].Name, SysStringLen(values[i].Name)));
            if (copied.name.empty())
                return E_INVALIDARG;
            copied.isFlags =
                detail::is_confirmed_xaml_flags_type(copied.name);

            const LONG memberCount =
                intsUpper >= intsLower ? intsUpper - intsLower + 1 : 0;
            copied.members.reserve(static_cast<size_t>(memberCount));
            for (LONG offset = 0; offset < memberCount; ++offset) {
                LONG intIndex = intsLower + offset;
                LONG stringIndex = stringsLower + offset;
                int machineValue = 0;
                RETURN_IF_FAILED(SafeArrayGetElement(
                    values[i].ValueInts, &intIndex, &machineValue));
                BSTR rawName = nullptr;
                const HRESULT stringHr = SafeArrayGetElement(
                    values[i].ValueStrings, &stringIndex, &rawName);
                if (FAILED(stringHr)) {
                    SysFreeString(rawName);
                    return stringHr;
                }
                wil::unique_bstr memberName(rawName);
                if (!memberName)
                    return E_INVALIDARG;
                copied.members.push_back({
                    static_cast<int32_t>(machineValue),
                    std::wstring(memberName.get(), SysStringLen(memberName.get())),
                });
            }
            parsedCatalog.push_back(std::move(copied));
        }
        output = std::move(parsedCatalog);
    } catch (const std::bad_alloc&) {
        output.clear();
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

} // namespace lvt::tap
