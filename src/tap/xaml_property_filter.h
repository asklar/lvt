#pragma once
// xaml_property_filter.h — pure decision logic for which properties the XAML
// TAP DLL captures from IVisualTreeService::GetPropertyValuesChain.
//
// Split out of lvt_tap.cpp so it can be unit tested without a live XAML
// process: this header has no COM/XAML dependency, only <string>.

#include <string>

namespace lvt {

// Free-text properties lvt captures when their value looks like a real string
// (see xaml_looks_like_handle / xaml_is_string_value_type).
inline bool xaml_is_text_property(const std::wstring& name) {
    return name == L"Text" || name == L"Content" || name == L"Header" ||
           name == L"PlaceholderText" || name == L"Description" ||
           name == L"Title" || name == L"Glyph";
}

// State/identity properties lvt captures regardless of value shape: their
// value being "0", or a long numeric string that would otherwise look like a
// handle, is legitimate data, not noise. xaml_should_capture_property skips
// the handle heuristic entirely for names in this list, rather than relying
// on ValueType the way text properties do — see its comment for why.
inline bool xaml_is_state_property(const std::wstring& name) {
    return name == L"AutomationProperties.Name" ||
           name == L"AutomationProperties.AutomationId" ||
           name == L"AutomationProperties.HelpText" ||
           name == L"IsEnabled" || name == L"Visibility" ||
           name == L"IsChecked" || name == L"IsSelected" ||
           name == L"IsOn" || name == L"Orientation" ||
           name == L"Source" || name == L"Tag";
}

// XAML diagnostics reports properties whose declared type is a real string
// ("String") or a value type serialized as text ("Boolean", "Int32",
// "Double", "Enum"). An empty/unknown ValueType is treated as string-shaped
// too — that is the permissive direction, deliberately: dropping a property
// just because its ValueType did not come through would lose real data, and
// xaml_looks_like_handle() is what actually guards against reference-typed
// values in that unknown-type case (see its comment).
inline bool xaml_is_string_value_type(const std::wstring& valueType) {
    return valueType == L"String" || valueType == L"" || valueType == L"Boolean" ||
           valueType == L"Int32" || valueType == L"Double" || valueType == L"Enum";
}

// XAML serializes reference-typed property values (anything that is not a
// primitive) as an opaque numeric handle ID, indistinguishable in shape from
// a caller's genuinely numeric-looking text — an order number, a phone
// number, a timestamp. The two cannot be told apart from the string alone.
//
// ValueType breaks the tie: when it is exactly "String", XAML has already
// told us this is real text, so the shape heuristic below must not veto it.
// Only when ValueType is empty/unknown (or anything other than a confirmed
// string) do we fall back to guessing from shape: a value that is all
// digits and longer than 10 characters is treated as a handle. 10 was
// chosen to clear any plausible phone number or small order ID while still
// catching XAML's handle IDs, which run much longer; it is a heuristic, not
// a guarantee, which is exactly why a confirmed ValueType always wins.
inline bool xaml_looks_like_handle(const std::wstring& value, const std::wstring& valueType) {
    if (valueType == L"String")
        return false;
    if (value.size() <= 10)
        return false;
    for (wchar_t c : value) {
        if (c < L'0' || c > L'9')
            return false;
    }
    return true;
}

// Decides whether a (name, value, valueType) triple from
// IVisualTreeService::GetPropertyValuesChain should be captured as an lvt
// property.
//
// "0" is NOT treated as an unset sentinel here, unlike some earlier lvt
// code: XAML diagnostics returns enums as their raw integer ordinal, and
// Visible (Windows.UI.Xaml.Visibility / Microsoft.UI.Xaml.Visibility) and
// Vertical (Orientation) are both legitimately 0. Filtering out the literal
// string "0" would silently drop those states on every element that has
// them — the common case, not the exceptional one — while leaving
// Collapsed/Horizontal, the less common values, untouched. An empty
// string is the only "nothing came back" signal this API actually gives us,
// so that is the only sentinel this function applies.
//
// Property-specific "0 vs absent" sentinels (the way uia_property_value_is_unset
// handles it per-property for the UIA provider) are not implemented here:
// the XAML diagnostics property chain has no equivalent "not supported"
// signal to disambiguate against, so there is nothing more precise to build
// on without a live property-by-property audit this function does not have
// the information to do safely.
//
// Forward-looking note for anyone extending isStateProp with a Double-valued
// layout property such as Width/Height/Margin: WPF and XAML spell "Auto"
// as NaN for those, so an isfinite() rejection (as used in the bounds-parsing
// path elsewhere in lvt_tap.cpp) would turn "Auto" into "absent" rather than
// into the distinct fourth state it actually is. This function stores values
// as raw strings and never parses them to a number, so that trap does not
// apply today — it only would if a future change starts parsing property
// values here the way the bounds path already does for ActualWidth/Height.
inline bool xaml_should_capture_property(const std::wstring& name, const std::wstring& value,
                                          const std::wstring& valueType) {
    if (value.empty())
        return false;
    // The handle heuristic only ever applies to text properties. State
    // properties (Tag, Source, AutomationProperties.*, Visibility, ...) are
    // captured regardless of value shape, per xaml_is_state_property's
    // contract: AutomationProperties.Name/AutomationId/HelpText are always
    // genuinely string-typed in XAML, so a long numeric AutomationId (a
    // very plausible generated id) must not be mistaken for a handle. Tag
    // and Source can hold arbitrary reference-typed values too, but this
    // function has no ValueType-independent way to tell "Tag holds a real
    // long numeric string" from "Tag holds a handle" once the pattern-backed
    // exemption above does not apply — capturing it as-is (possibly a raw
    // handle string) is the lesser failure next to silently dropping an
    // AutomationId, which is the property this list exists to protect.
    if (xaml_is_text_property(name)) {
        if (xaml_looks_like_handle(value, valueType))
            return false;
        return xaml_is_string_value_type(valueType);
    }
    return xaml_is_state_property(name);
}

} // namespace lvt
