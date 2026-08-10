#pragma once
#include <string>
#include <vector>

namespace lvt {

// Which UIA tree view to walk. UIA exposes three: raw (every element, including
// framework scaffolding), control (elements a user can interact with), and
// content (elements conveying information). Control is the usual automation view
// and lvt's default.
enum class UiaView { raw, control, content };

// Parse "raw" | "control" | "content". Returns false on anything else.
bool parse_uia_view(const std::string& text, UiaView& out);
const char* uia_view_name(UiaView view);

// Resolve a UIA property by the name lvt emits (e.g. "AutomationId",
// "HelpText"). Returns 0 if unknown. Matching is case-insensitive, and the
// "UIA_"/"...PropertyId" decorations are optional, so "automationid" and
// "UIA_AutomationIdPropertyId" both resolve.
long uia_property_id(const std::string& name);

// The name lvt emits for a property id, or an empty string if not one we name.
std::string uia_property_name(long propertyId);

// Properties fetched for every element: identity, control classification, and
// interaction state. Cached in one batched request, so this set is also the
// cost floor of a walk.
const std::vector<long>& uia_core_property_ids();

// True when a value equals the "unset" sentinel for that property (e.g. Level 0,
// NativeWindowHandle 0x0), so optional metadata is only emitted where a provider
// actually set it.
//
// Distinct from "the element does not support this property", which UIA answers
// itself via GetCachedPropertyValueEx(ignoreDefaultValue=TRUE).
bool uia_property_value_is_unset(long propertyId, const std::string& value);

// Human-readable name for a UIA_*ControlTypeId, e.g. "Button". Falls back to
// "ControlType(<id>)" so an unrecognised control type is still identifiable.
std::string uia_control_type_name(long controlTypeId);

// True if the property carries an enumeration rather than a plain scalar.
bool uia_property_is_enum(long propertyId);

// Render an enum-valued UIA property.
//
// Known values use the documented UIA enumerator name ("On", "Collapsed",
// "ReadyForUserInteraction"). The property key already names the enum
// (Toggle.ToggleState="On"), so the value is not redundantly re-qualified.
//
// Unknown values render as "EnumName(<n>)" — that is where naming the enum
// earns its keep, because a bare integer would tell a reader neither what the
// number means nor that lvt failed to recognise it.
//
// Returns an empty string if the property is not enum-valued.
std::string uia_enum_value_name(long propertyId, long value);

// Render an LCID (the Culture property) as a BCP-47 locale name such as
// "en-US". Falls back to the numeric LCID when it cannot be resolved.
std::string uia_culture_name(long lcid);

// Human-readable name for a supported pattern id, e.g. "Invoke". Empty if
// unknown.
std::string uia_pattern_name(long patternId);

// Pattern ids probed on every element to populate the SupportedPatterns
// property and the pattern-state properties derived from them.
const std::vector<long>& uia_probed_pattern_ids();

// Resolve a pattern by name (case-insensitive, "Pattern" suffix optional), e.g.
// "Invoke", "TogglePattern". Returns 0 if unknown.
long uia_pattern_id(const std::string& name);

} // namespace lvt
