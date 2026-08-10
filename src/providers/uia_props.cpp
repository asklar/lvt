#include "uia_props.h"

#include <Windows.h>
#include <oleacc.h>
#include <UIAutomation.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace lvt {
namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Accept "AutomationId", "automationid", and "UIA_AutomationIdPropertyId" alike,
// so callers can paste names straight out of the UIA docs or out of lvt output.
std::string normalize_property_name(const std::string& name) {
    auto s = to_lower(name);
    if (s.rfind("uia_", 0) == 0)
        s.erase(0, 4);
    const std::string suffix = "propertyid";
    if (s.size() > suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
        s.erase(s.size() - suffix.size());
    return s;
}

struct PropertyEntry {
    const char* name;
    long id;
    // Value that means "unset" for this property, so optional metadata only
    // appears where a provider actually set it. Comma-separated because
    // frameworks disagree on the sentinel: Win32 reports 0 for an unset
    // Level/PositionInSet/SizeOfSet where XAML reports -1.
    //
    // This is a different concern from "the element does not support this
    // property at all" — that is answered by UIA itself via
    // GetCachedPropertyValueEx(ignoreDefaultValue=TRUE), not by this table.
    const char* unsetValues;
};

// The name on the left is what lvt emits, chosen to match the UIA property name
// without its decoration. element_key.cpp already prefers a property literally
// called "AutomationId" when building durable keys, so spelling it exactly that
// way makes durable keys work on UIA trees with no extra plumbing.
const PropertyEntry kProperties[] = {
    // Identity
    {"AutomationId",         UIA_AutomationIdPropertyId, nullptr},
    {"Name",                 UIA_NamePropertyId, nullptr},
    {"ClassName",            UIA_ClassNamePropertyId, nullptr},
    {"RuntimeId",            UIA_RuntimeIdPropertyId, nullptr},
    {"FrameworkId",          UIA_FrameworkIdPropertyId, nullptr},
    {"ProcessId",            UIA_ProcessIdPropertyId, nullptr},
    {"NativeWindowHandle",   UIA_NativeWindowHandlePropertyId, "0x0"},
    {"ProviderDescription",  UIA_ProviderDescriptionPropertyId, nullptr},

    // Classification
    {"ControlType",          UIA_ControlTypePropertyId, nullptr},
    {"LocalizedControlType", UIA_LocalizedControlTypePropertyId, nullptr},
    {"ItemType",             UIA_ItemTypePropertyId, nullptr},
    {"ItemStatus",           UIA_ItemStatusPropertyId, nullptr},
    {"HelpText",             UIA_HelpTextPropertyId, nullptr},
    {"AcceleratorKey",       UIA_AcceleratorKeyPropertyId, nullptr},
    {"AccessKey",            UIA_AccessKeyPropertyId, nullptr},
    {"FullDescription",      UIA_FullDescriptionPropertyId, nullptr},

    // State
    {"IsEnabled",            UIA_IsEnabledPropertyId, nullptr},
    {"IsOffscreen",          UIA_IsOffscreenPropertyId, nullptr},
    {"IsKeyboardFocusable",  UIA_IsKeyboardFocusablePropertyId, nullptr},
    {"HasKeyboardFocus",     UIA_HasKeyboardFocusPropertyId, nullptr},
    {"IsContentElement",     UIA_IsContentElementPropertyId, nullptr},
    {"IsControlElement",     UIA_IsControlElementPropertyId, nullptr},
    {"IsPassword",           UIA_IsPasswordPropertyId, "false"},
    {"IsDialog",             UIA_IsDialogPropertyId, "false"},
    {"BoundingRectangle",    UIA_BoundingRectanglePropertyId, nullptr},
    {"Culture",              UIA_CulturePropertyId, "0"},
    {"Orientation",          UIA_OrientationPropertyId, "None"},
    {"LiveSetting",          UIA_LiveSettingPropertyId, "0"},
    {"Level",                UIA_LevelPropertyId, "0,-1"},
    {"PositionInSet",        UIA_PositionInSetPropertyId, "0,-1"},
    {"SizeOfSet",            UIA_SizeOfSetPropertyId, "0,-1"},
    {"LandmarkType",         UIA_LandmarkTypePropertyId, "0"},
    {"LocalizedLandmarkType", UIA_LocalizedLandmarkTypePropertyId, nullptr},

    // Pattern-backed values. Fetching these as properties rather than through
    // the pattern interfaces keeps them inside the single batched cache request;
    // UIA reports them as "not supported" on elements lacking the pattern.
    {"Value.Value",              UIA_ValueValuePropertyId, nullptr},
    {"Value.IsReadOnly",         UIA_ValueIsReadOnlyPropertyId, nullptr},
    {"Toggle.ToggleState",       UIA_ToggleToggleStatePropertyId, nullptr},
    {"ExpandCollapse.State",     UIA_ExpandCollapseExpandCollapseStatePropertyId, nullptr},
    {"SelectionItem.IsSelected", UIA_SelectionItemIsSelectedPropertyId, nullptr},
    {"RangeValue.Value",         UIA_RangeValueValuePropertyId, nullptr},
    {"RangeValue.Minimum",       UIA_RangeValueMinimumPropertyId, nullptr},
    {"RangeValue.Maximum",       UIA_RangeValueMaximumPropertyId, nullptr},
    {"RangeValue.IsReadOnly",    UIA_RangeValueIsReadOnlyPropertyId, nullptr},
    {"Scroll.HorizontalPercent", UIA_ScrollHorizontalScrollPercentPropertyId, nullptr},
    {"Scroll.VerticalPercent",   UIA_ScrollVerticalScrollPercentPropertyId, nullptr},
    {"Scroll.HorizontallyScrollable", UIA_ScrollHorizontallyScrollablePropertyId, nullptr},
    {"Scroll.VerticallyScrollable",   UIA_ScrollVerticallyScrollablePropertyId, nullptr},
    {"Window.CanMaximize",       UIA_WindowCanMaximizePropertyId, nullptr},
    {"Window.CanMinimize",       UIA_WindowCanMinimizePropertyId, nullptr},
    {"Window.IsModal",           UIA_WindowIsModalPropertyId, nullptr},
    {"Window.WindowVisualState", UIA_WindowWindowVisualStatePropertyId, nullptr},
    {"Window.WindowInteractionState", UIA_WindowWindowInteractionStatePropertyId, nullptr},
    {"Grid.RowCount",            UIA_GridRowCountPropertyId, nullptr},
    {"Grid.ColumnCount",         UIA_GridColumnCountPropertyId, nullptr},
    {"GridItem.Row",             UIA_GridItemRowPropertyId, nullptr},
    {"GridItem.Column",          UIA_GridItemColumnPropertyId, nullptr},
    {"Table.RowOrColumnMajor",   UIA_TableRowOrColumnMajorPropertyId, nullptr},
    {"Selection.CanSelectMultiple", UIA_SelectionCanSelectMultiplePropertyId, nullptr},
    {"Selection.IsSelectionRequired", UIA_SelectionIsSelectionRequiredPropertyId, nullptr},
    {"Transform.CanMove",        UIA_TransformCanMovePropertyId, nullptr},
    {"Transform.CanResize",      UIA_TransformCanResizePropertyId, nullptr},
    {"Transform.CanRotate",      UIA_TransformCanRotatePropertyId, nullptr},
};

struct ControlTypeEntry {
    const char* name;
    long id;
};

const ControlTypeEntry kControlTypes[] = {
    {"Button", UIA_ButtonControlTypeId},
    {"Calendar", UIA_CalendarControlTypeId},
    {"CheckBox", UIA_CheckBoxControlTypeId},
    {"ComboBox", UIA_ComboBoxControlTypeId},
    {"Edit", UIA_EditControlTypeId},
    {"Hyperlink", UIA_HyperlinkControlTypeId},
    {"Image", UIA_ImageControlTypeId},
    {"ListItem", UIA_ListItemControlTypeId},
    {"List", UIA_ListControlTypeId},
    {"Menu", UIA_MenuControlTypeId},
    {"MenuBar", UIA_MenuBarControlTypeId},
    {"MenuItem", UIA_MenuItemControlTypeId},
    {"ProgressBar", UIA_ProgressBarControlTypeId},
    {"RadioButton", UIA_RadioButtonControlTypeId},
    {"ScrollBar", UIA_ScrollBarControlTypeId},
    {"Slider", UIA_SliderControlTypeId},
    {"Spinner", UIA_SpinnerControlTypeId},
    {"StatusBar", UIA_StatusBarControlTypeId},
    {"Tab", UIA_TabControlTypeId},
    {"TabItem", UIA_TabItemControlTypeId},
    {"Text", UIA_TextControlTypeId},
    {"ToolBar", UIA_ToolBarControlTypeId},
    {"ToolTip", UIA_ToolTipControlTypeId},
    {"Tree", UIA_TreeControlTypeId},
    {"TreeItem", UIA_TreeItemControlTypeId},
    {"Custom", UIA_CustomControlTypeId},
    {"Group", UIA_GroupControlTypeId},
    {"Thumb", UIA_ThumbControlTypeId},
    {"DataGrid", UIA_DataGridControlTypeId},
    {"DataItem", UIA_DataItemControlTypeId},
    {"Document", UIA_DocumentControlTypeId},
    {"SplitButton", UIA_SplitButtonControlTypeId},
    {"Window", UIA_WindowControlTypeId},
    {"Pane", UIA_PaneControlTypeId},
    {"Header", UIA_HeaderControlTypeId},
    {"HeaderItem", UIA_HeaderItemControlTypeId},
    {"Table", UIA_TableControlTypeId},
    {"TitleBar", UIA_TitleBarControlTypeId},
    {"Separator", UIA_SeparatorControlTypeId},
    {"SemanticZoom", UIA_SemanticZoomControlTypeId},
    {"AppBar", UIA_AppBarControlTypeId},
};

struct PatternEntry {
    const char* name;
    long id;
};

const PatternEntry kPatterns[] = {
    {"Invoke", UIA_InvokePatternId},
    {"Selection", UIA_SelectionPatternId},
    {"Value", UIA_ValuePatternId},
    {"RangeValue", UIA_RangeValuePatternId},
    {"Scroll", UIA_ScrollPatternId},
    {"ExpandCollapse", UIA_ExpandCollapsePatternId},
    {"Grid", UIA_GridPatternId},
    {"GridItem", UIA_GridItemPatternId},
    {"MultipleView", UIA_MultipleViewPatternId},
    {"Window", UIA_WindowPatternId},
    {"SelectionItem", UIA_SelectionItemPatternId},
    {"Dock", UIA_DockPatternId},
    {"Table", UIA_TablePatternId},
    {"TableItem", UIA_TableItemPatternId},
    {"Text", UIA_TextPatternId},
    {"Toggle", UIA_TogglePatternId},
    {"Transform", UIA_TransformPatternId},
    {"ScrollItem", UIA_ScrollItemPatternId},
    {"LegacyIAccessible", UIA_LegacyIAccessiblePatternId},
    {"ItemContainer", UIA_ItemContainerPatternId},
    {"VirtualizedItem", UIA_VirtualizedItemPatternId},
    {"SynchronizedInput", UIA_SynchronizedInputPatternId},
};

const std::unordered_map<std::string, long>& property_lookup() {
    static const auto* map = [] {
        auto* m = new std::unordered_map<std::string, long>();
        for (const auto& e : kProperties)
            m->emplace(normalize_property_name(e.name), e.id);
        return m;
    }();
    return *map;
}

const std::unordered_map<long, std::string>& property_names() {
    static const auto* map = [] {
        auto* m = new std::unordered_map<long, std::string>();
        for (const auto& e : kProperties)
            m->emplace(e.id, e.name);
        return m;
    }();
    return *map;
}

} // namespace

bool parse_uia_view(const std::string& text, UiaView& out) {
    auto s = to_lower(text);
    if (s == "raw")     { out = UiaView::raw;     return true; }
    if (s == "control") { out = UiaView::control; return true; }
    if (s == "content") { out = UiaView::content; return true; }
    return false;
}

const char* uia_view_name(UiaView view) {
    switch (view) {
    case UiaView::raw:     return "raw";
    case UiaView::control: return "control";
    case UiaView::content: return "content";
    }
    return "control";
}

long uia_property_id(const std::string& name) {
    const auto& map = property_lookup();
    auto it = map.find(normalize_property_name(name));
    return it == map.end() ? 0 : it->second;
}

std::string uia_property_name(long propertyId) {
    const auto& map = property_names();
    auto it = map.find(propertyId);
    return it == map.end() ? std::string() : it->second;
}

const std::vector<long>& uia_core_property_ids() {
    static const auto* ids = [] {
        auto* v = new std::vector<long>();
        v->reserve(std::size(kProperties));
        for (const auto& e : kProperties) {
            // ProviderDescription is long and mostly useful when debugging a
            // provider chain, so it is opt-in via --uia-props.
            if (e.id == UIA_ProviderDescriptionPropertyId)
                continue;
            v->push_back(e.id);
        }
        return v;
    }();
    return *ids;
}

bool uia_property_value_is_unset(long propertyId, const std::string& value) {
    for (const auto& e : kProperties) {
        if (e.id != propertyId)
            continue;
        if (!e.unsetValues)
            return false;
        const std::string sentinels = e.unsetValues;
        size_t start = 0;
        while (start <= sentinels.size()) {
            const size_t comma = sentinels.find(',', start);
            const auto piece = sentinels.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            if (piece == value)
                return true;
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return false;
    }
    return false;
}

std::string uia_control_type_name(long controlTypeId) {
    for (const auto& e : kControlTypes) {
        if (e.id == controlTypeId)
            return e.name;
    }
    return "ControlType(" + std::to_string(controlTypeId) + ")";
}

std::string uia_pattern_name(long patternId) {
    for (const auto& e : kPatterns) {
        if (e.id == patternId)
            return e.name;
    }
    return {};
}

const std::vector<long>& uia_probed_pattern_ids() {
    static const auto* ids = [] {
        auto* v = new std::vector<long>();
        v->reserve(std::size(kPatterns));
        for (const auto& e : kPatterns)
            v->push_back(e.id);
        return v;
    }();
    return *ids;
}

long uia_pattern_id(const std::string& name) {
    auto s = to_lower(name);
    const std::string suffix = "pattern";
    if (s.size() > suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
        s.erase(s.size() - suffix.size());
    for (const auto& e : kPatterns) {
        if (to_lower(e.name) == s)
            return e.id;
    }
    return 0;
}

} // namespace lvt
