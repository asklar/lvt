#include "element_key.h"
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <map>
#include <sstream>

namespace lvt {

bool parse_compact_xaml_key(const std::string& text, CompactXamlKey& out,
                            std::string& error) {
    std::string framework;
    size_t digitsStart = 0;
    if (text.rfind("xaml:0x", 0) == 0) {
        framework = "xaml";
        digitsStart = 7;
    } else if (text.rfind("winui3:0x", 0) == 0) {
        framework = "winui3";
        digitsStart = 9;
    } else {
        error = "only compact XAML/WinUI3 keys (xaml:0xHANDLE or "
                "winui3:0xHANDLE) support native property operations";
        return false;
    }

    const char* first = text.data() + digitsStart;
    const char* last = text.data() + text.size();
    if (first == last) {
        error = "XAML element key is missing its hexadecimal instance handle";
        return false;
    }

    uint64_t handle = 0;
    const auto parsed = std::from_chars(first, last, handle, 16);
    if (parsed.ec != std::errc() || parsed.ptr != last || handle == 0) {
        error = "XAML element key has an invalid hexadecimal instance handle";
        return false;
    }

    out.framework = std::move(framework);
    out.handle = handle;
    return true;
}

std::string escape_key_part(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '|' || c == '/' || c == ':')
            result += '\\';
        result += c;
    }
    return result;
}

std::string base_identity_key(const Element& el) {
    // Only one of type/className, not both: for every provider that sets
    // both (xaml_diag_common.cpp, wpf_inject.cpp), `type` is derived as the
    // substring of `className` after its last '.', so it never carries
    // information className does not already have — including both here
    // duplicated it for nothing. className is the more specific of the two
    // when both exist (a raw win32/native class name, or a fully-qualified
    // XAML type), so it wins; type is the fallback for providers where
    // className can be legitimately empty (UIA elements often report no
    // ClassName at all, see uia_provider.cpp), so the key never gets an
    // empty identity segment.
    //
    // This matters far more than it looks: a key is built once per element
    // but then repeated as a "/"-joined prefix in *every one* of that
    // element's descendants (see assign_child_keys below), so halving one
    // segment here roughly halves the key payload of an entire subtree, not
    // just one element. Measured on a real ~1900-element WinUI3 tree
    // (Microsoft Store), the full ancestor-chain "key" made up 40% of the
    // dump's total JSON size before this change.
    const std::string& identity = el.className.empty() ? el.type : el.className;
    return escape_key_part(el.framework) + "|" + escape_key_part(identity);
}

static std::string hwnd_key(uintptr_t handle) {
    std::ostringstream out;
    out << "hwnd:0x" << std::hex << std::uppercase << handle;
    return out.str();
}

static std::string provider_handle_key(uint64_t handle) {
    std::ostringstream out;
    out << "provider:0x" << std::hex << std::uppercase << handle;
    return out.str();
}

bool has_process_wide_provider_identity(const Element& el) {
    return el.providerHandle != 0 &&
           (el.framework == "xaml" || el.framework == "winui3");
}

bool has_durable_provider_identity(const Element& el) {
    return el.providerHandle != 0 &&
           el.framework != "win32" && el.framework != "comctl";
}

// Framework-native handles are already object identities. XAML/WinUI handles
// are runtime InstanceHandles; WPF/WinForms handles come from their persistent
// managed object registries. Only XAML/WinUI opt into process-wide reparent
// reconciliation; managed handles are session-scoped. Win32/common-control
// HWNDs use nativeHandle so their keys do not depend on a property connection.
static std::string compact_instance_key(const Element& el) {
    uint64_t handle = 0;
    if ((el.framework == "win32" || el.framework == "comctl") &&
        el.nativeHandle != 0) {
        handle = static_cast<uint64_t>(el.nativeHandle);
    } else if (el.providerHandle != 0 &&
               (el.framework == "xaml" || el.framework == "winui3" ||
                el.framework == "wpf" || el.framework == "winforms")) {
        handle = el.providerHandle;
    } else {
        return {};
    }

    std::ostringstream out;
    out << el.framework << ":0x" << std::hex << std::uppercase << handle;
    return out.str();
}

std::string stable_name_key(const Element& el) {
    if (!el.durableIdentity.empty())
        return "identity:" + escape_key_part(el.durableIdentity);

    for (const char* name : {"AutomationId", "x:Name", "Name", "automationId", "name"}) {
        auto it = el.properties.find(name);
        if (it != el.properties.end() && !it->second.empty())
            return std::string(name) + ":" + escape_key_part(it->second);
    }
    return {};
}

// baseCounts/hwndCounts/nameCounts are all scoped to just `parent`'s own
// direct children (see assign_child_keys, which builds them fresh for each
// parent) — never counted across the whole tree. This locality is the
// entire point: a node's key can only ever be disturbed by a change among
// its OWN siblings, never by something elsewhere in an unrelated subtree.
// See assign_element_keys' doc comment for what used to go wrong when a
// different, GLOBALLY-scoped algorithm was used instead (in watch's
// diffing, before this).
static std::string discriminator_for_child(const Element& child, size_t childIndex,
                                           const std::map<std::string, int>& baseCounts,
                                           const std::map<std::string, int>& providerCounts,
                                           const std::map<std::string, int>& hwndCounts,
                                           const std::map<std::string, int>& nameCounts) {
    auto base = base_identity_key(child);
    if (has_durable_provider_identity(child)) {
        auto provider = provider_handle_key(child.providerHandle);
        auto full = base + "|" + provider;
        auto count = providerCounts.find(full);
        if (count == providerCounts.end() || count->second == 1)
            return full;
    }
    if (child.nativeHandle != 0) {
        auto hwnd = hwnd_key(child.nativeHandle);
        auto full = base + "|" + hwnd;
        auto count = hwndCounts.find(full);
        if (count == hwndCounts.end() || count->second == 1)
            return full;
    }

    auto name = stable_name_key(child);
    if (!name.empty()) {
        auto full = base + "|" + name;
        auto count = nameCounts.find(full);
        if (count == nameCounts.end() || count->second == 1)
            return full;
    }

    auto baseIt = baseCounts.find(base);
    if (baseIt == baseCounts.end() || baseIt->second <= 1)
        return base;

    return base + "|@" + std::to_string(childIndex);
}

static void assign_child_keys(Element& parent, const std::string& parentKey) {
    std::map<std::string, int> baseCounts;
    std::map<std::string, int> providerCounts;
    std::map<std::string, int> hwndCounts;
    std::map<std::string, int> nameCounts;

    for (const auto& child : parent.children) {
        auto base = base_identity_key(child);
        baseCounts[base]++;
        if (has_durable_provider_identity(child))
            providerCounts[base + "|" + provider_handle_key(child.providerHandle)]++;
        if (child.nativeHandle != 0)
            hwndCounts[base + "|" + hwnd_key(child.nativeHandle)]++;
        auto name = stable_name_key(child);
        if (!name.empty())
            nameCounts[base + "|" + name]++;
    }

    for (size_t i = 0; i < parent.children.size(); ++i) {
        auto& child = parent.children[i];
        auto compact = compact_instance_key(child);
        if (!compact.empty()) {
            child.key = std::move(compact);
        } else {
            auto segment = discriminator_for_child(
                child, i, baseCounts, providerCounts, hwndCounts, nameCounts);
            child.key = parentKey.empty() ? segment : parentKey + "/" + segment;
        }
        assign_child_keys(child, child.key);
    }
}

void assign_element_keys(Element& root, bool preserveExistingRoot) {
    if (!preserveExistingRoot || root.key.empty()) {
        root.key = compact_instance_key(root);
        if (root.key.empty())
            root.key = base_identity_key(root);
    }
    assign_child_keys(root, root.key);
}

} // namespace lvt
