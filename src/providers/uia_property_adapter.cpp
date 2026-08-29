#include "uia_property_adapter.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <set>
#include <sstream>
#include <string_view>

namespace lvt {
namespace {

bool parse_bool(const Element& element, const char* name, bool fallback) {
    const auto found = element.properties.find(name);
    if (found == element.properties.end())
        return fallback;
    return found->second == "true";
}

std::optional<double> parse_number(const Element& element, const char* name) {
    const auto found = element.properties.find(name);
    if (found == element.properties.end() || found->second.empty())
        return std::nullopt;
    char* end = nullptr;
    const auto value = std::strtod(found->second.c_str(), &end);
    if (end == found->second.c_str() || *end != '\0' || !std::isfinite(value))
        return std::nullopt;
    return value;
}

std::string property_value(const Element& element, const char* name) {
    const auto found = element.properties.find(name);
    return found == element.properties.end() ? std::string() : found->second;
}

std::set<std::string> supported_patterns(const Element& element) {
    std::set<std::string> patterns;
    const auto value = property_value(element, "SupportedPatterns");
    size_t start = 0;
    while (start < value.size()) {
        const auto comma = value.find(',', start);
        const auto token = value.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!token.empty())
            patterns.insert(token);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return patterns;
}

std::string normalized_patterns(const std::set<std::string>& patterns) {
    std::string result;
    for (const auto& pattern : patterns) {
        if (!result.empty())
            result += ',';
        result += pattern;
    }
    return result;
}

std::string format_number(double value) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

uint64_t fnv1a(std::string_view text) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto ch : text) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex_hash(std::string_view text) {
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%016llX",
             static_cast<unsigned long long>(fnv1a(text)));
    return buffer;
}

struct SchemaFacts {
    std::string controlType;
    std::set<std::string> patterns;
    bool enabled = true;
    bool valueReadOnly = true;
    bool rangeReadOnly = true;
    std::optional<double> rangeMinimum;
    std::optional<double> rangeMaximum;
    bool expandLeaf = false;
    UiaSelectionCapabilities selection;
    bool horizontalScroll = false;
    bool verticalScroll = false;
};

SchemaFacts schema_facts(
    const Element& element, const UiaSelectionCapabilities& selection) {
    SchemaFacts facts;
    facts.controlType = element.type;
    facts.patterns = supported_patterns(element);
    facts.enabled = parse_bool(element, "IsEnabled", true);
    facts.valueReadOnly = parse_bool(element, "Value.IsReadOnly", true);
    facts.rangeReadOnly = parse_bool(element, "RangeValue.IsReadOnly", true);
    facts.rangeMinimum = parse_number(element, "RangeValue.Minimum");
    facts.rangeMaximum = parse_number(element, "RangeValue.Maximum");
    facts.expandLeaf =
        property_value(element, "ExpandCollapse.State") == "LeafNode";
    facts.selection = selection;
    facts.horizontalScroll =
        parse_bool(element, "Scroll.HorizontallyScrollable", false);
    facts.verticalScroll =
        parse_bool(element, "Scroll.VerticallyScrollable", false);
    return facts;
}

std::string schema_key(const SchemaFacts& facts) {
    std::ostringstream key;
    key << "type=" << facts.controlType
        << "|patterns=" << normalized_patterns(facts.patterns)
        << "|enabled=" << facts.enabled
        << "|valueReadOnly=" << facts.valueReadOnly
        << "|rangeReadOnly=" << facts.rangeReadOnly
        << "|rangeMin=";
    if (facts.rangeMinimum)
        key << format_number(*facts.rangeMinimum);
    key << "|rangeMax=";
    if (facts.rangeMaximum)
        key << format_number(*facts.rangeMaximum);
    key << "|expandLeaf=" << facts.expandLeaf
        << "|selectionKnown=" << facts.selection.known
        << "|selectionMultiple=" << facts.selection.canSelectMultiple
        << "|selectionRequired=" << facts.selection.isSelectionRequired
        << "|horizontalScroll=" << facts.horizontalScroll
        << "|verticalScroll=" << facts.verticalScroll;
    return key.str();
}

bool has_pattern(const SchemaFacts& facts, const char* name) {
    return facts.patterns.contains(name);
}

PropertyDescriptor descriptor(
    const std::string& schemaHash, size_t ordinal,
    std::string name, std::string displayName,
    std::string declaringType, std::string propertyType,
    PropertyEditorKind kind, bool writable, std::string description) {
    PropertyDescriptor result;
    result.descriptorId =
        "uia:" + schemaHash + ":" + std::to_string(ordinal);
    result.name = std::move(name);
    result.displayName = std::move(displayName);
    result.provider = "uia";
    result.framework = "uia";
    result.declaringType = std::move(declaringType);
    result.propertyType = std::move(propertyType);
    result.kind = kind;
    result.writable = writable;
    result.supportsClear = false;
    result.description = std::move(description);
    return result;
}

std::shared_ptr<const PropertySchema> build_schema(
    const SchemaFacts& facts, const std::string& key) {
    auto schema = std::make_shared<PropertySchema>();
    const auto hash = hex_hash(key);
    schema->schemaId = "uia:" + hash;

    const auto add = [&](PropertyDescriptor item) {
        schema->descriptors.push_back(std::move(item));
    };

    if (has_pattern(facts, "Value")) {
        const bool writable = facts.enabled && !facts.valueReadOnly;
        add(descriptor(
            hash, schema->descriptors.size(),
            "Value.Value", "Value", "UIAutomation.ValuePattern", "String",
            writable ? PropertyEditorKind::string : PropertyEditorKind::readonly,
            writable,
            !facts.enabled
                ? "The UI Automation element is disabled."
                : facts.valueReadOnly
                ? "The UI Automation Value pattern reports this value as read-only."
                : "Set through the UI Automation Value pattern."));
    }

    if (has_pattern(facts, "RangeValue")) {
        const bool hasBounds =
            facts.rangeMinimum.has_value() && facts.rangeMaximum.has_value();
        const bool writable = facts.enabled && !facts.rangeReadOnly && hasBounds;
        auto item = descriptor(
            hash, schema->descriptors.size(),
            "RangeValue.Value", "Range value",
            "UIAutomation.RangeValuePattern", "Double",
            writable ? PropertyEditorKind::number : PropertyEditorKind::readonly,
            writable,
            !facts.enabled
                ? "The UI Automation element is disabled."
                : facts.rangeReadOnly
                ? "The UI Automation RangeValue pattern reports this value as read-only."
                : !hasBounds
                    ? "The provider did not supply both RangeValue bounds."
                    : "Set through the UI Automation RangeValue pattern.");
        item.minimum = facts.rangeMinimum;
        item.maximum = facts.rangeMaximum;
        // UIA does expose SmallChange, but lvt's cached property set does not
        // currently query it. Leaving step absent is intentional: a made-up
        // increment would be provider inference rather than metadata.
        add(std::move(item));
    }

    if (has_pattern(facts, "Toggle")) {
        const bool writable = facts.enabled;
        auto item = descriptor(
            hash, schema->descriptors.size(),
            "Toggle.ToggleState", "Toggle state",
            "UIAutomation.TogglePattern", "ToggleState",
            writable ? PropertyEditorKind::enumeration : PropertyEditorKind::readonly,
            writable,
            writable
                ? "Choose a state; the provider drives TogglePattern until that exact state is reached."
                : "The UI Automation element is disabled.");
        if (writable) {
            // UIA has no capability property that distinguishes two-state from
            // three-state toggles. Off and On are the only states every Toggle
            // provider can intentionally set without probing by mutation.
            item.choices = {{"Off", "Off"}, {"On", "On"}};
        }
        add(std::move(item));
    }

    if (has_pattern(facts, "ExpandCollapse")) {
        const bool writable = facts.enabled && !facts.expandLeaf;
        auto item = descriptor(
            hash, schema->descriptors.size(),
            "ExpandCollapse.State", "Expanded state",
            "UIAutomation.ExpandCollapsePattern", "ExpandCollapseState",
            writable ? PropertyEditorKind::enumeration : PropertyEditorKind::readonly,
            writable,
            !facts.enabled
                ? "The UI Automation element is disabled."
                : facts.expandLeaf
                ? "Leaf nodes cannot be expanded or collapsed."
                : "Choose Expanded or Collapsed; partially expanded controls are driven to the selected state.");
        if (writable)
            item.choices = {{"Expanded", "Expanded"}, {"Collapsed", "Collapsed"}};
        add(std::move(item));
    }

    if (has_pattern(facts, "SelectionItem")) {
        std::vector<PropertyChoice> choices{{"select", "Replace selection"}};
        if (facts.selection.known && facts.selection.canSelectMultiple)
            choices.push_back({"add", "Add to selection"});
        if (facts.selection.known && !facts.selection.isSelectionRequired)
            choices.push_back({"remove", "Remove from selection"});
        const bool writable = facts.enabled && !choices.empty();
        auto item = descriptor(
            hash, schema->descriptors.size(),
            "SelectionItem.Action", "Selection",
            "UIAutomation.SelectionItemPattern", "Command",
            writable ? PropertyEditorKind::command : PropertyEditorKind::readonly,
            writable,
            facts.selection.known
                ? "Selection commands reflect the Selection container's capabilities."
                : "The selection container was not available; only replacement selection is advertised.");
        if (writable)
            item.choices = std::move(choices);
        add(std::move(item));
    }

    if (has_pattern(facts, "Scroll")) {
        std::vector<PropertyChoice> choices;
        if (facts.verticalScroll) {
            choices.push_back({"up", "Scroll up"});
            choices.push_back({"down", "Scroll down"});
        }
        if (facts.horizontalScroll) {
            choices.push_back({"left", "Scroll left"});
            choices.push_back({"right", "Scroll right"});
        }
        const bool writable = facts.enabled && !choices.empty();
        auto item = descriptor(
            hash, schema->descriptors.size(),
            "Scroll.Action", "Scroll",
            "UIAutomation.ScrollPattern", "Command",
            writable ? PropertyEditorKind::command : PropertyEditorKind::readonly,
            writable,
            writable
                ? "Only directions the UI Automation Scroll pattern reports as supported are offered."
                : "The Scroll pattern reports no supported direction.");
        if (writable)
            item.choices = std::move(choices);
        add(std::move(item));
    }

    return schema;
}

PropertyValue make_value(
    const PropertyDescriptor& descriptor, const Element& element) {
    PropertyValue value;
    value.descriptorId = descriptor.descriptorId;
    value.runtimeType = descriptor.propertyType;
    value.source = descriptor.declaringType;

    if (descriptor.name == "SelectionItem.Action") {
        value.value = parse_bool(element, "SelectionItem.IsSelected", false)
            ? "Selected"
            : "Not selected";
    } else if (descriptor.name == "Scroll.Action") {
        const auto horizontal =
            property_value(element, "Scroll.HorizontalPercent");
        const auto vertical =
            property_value(element, "Scroll.VerticalPercent");
        if (!horizontal.empty())
            value.value = "Horizontal " + horizontal + "%";
        if (!vertical.empty()) {
            if (!value.value.empty())
                value.value += ", ";
            value.value += "Vertical " + vertical + "%";
        }
    } else {
        value.value = property_value(element, descriptor.name.c_str());
    }

    if (!descriptor.writable)
        value.readOnlyReason = descriptor.description;
    return value;
}

} // namespace

std::shared_ptr<const PropertySchema> UiaPropertySchemaCache::get_or_create(
    const Element& element, const UiaSelectionCapabilities& selection) {
    const auto facts = schema_facts(element, selection);
    const auto key = schema_key(facts);
    const auto found = m_schemas.find(key);
    if (found != m_schemas.end()) {
        found->second.lastUsed = ++m_clock;
        return found->second.schema;
    }
    auto schema = build_schema(facts, key);
    m_schemas.emplace(
        key, CachedSchema{.schema = schema, .lastUsed = ++m_clock});
    trim();
    return schema;
}

void UiaPropertySchemaCache::trim() {
    while (m_schemas.size() > kMaximumSchemas) {
        auto oldest = m_schemas.end();
        for (auto it = m_schemas.begin(); it != m_schemas.end(); ++it) {
            if (oldest == m_schemas.end() ||
                it->second.lastUsed < oldest->second.lastUsed) {
                oldest = it;
            }
        }
        if (oldest == m_schemas.end())
            break;
        m_schemas.erase(oldest);
    }
}

PropertySnapshotResult make_uia_property_snapshot(
    const Element& element,
    const UiaSelectionCapabilities& selection,
    UiaPropertySchemaCache& cache) {
    PropertySnapshotResult result;
    result.schema = cache.get_or_create(element, selection);
    result.values.reserve(result.schema->descriptors.size());
    for (const auto& descriptor : result.schema->descriptors)
        result.values.push_back(make_value(descriptor, element));
    result.ok = true;
    result.hresult = S_OK;
    result.error.clear();
    return result;
}

} // namespace lvt
