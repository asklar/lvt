#include "element_key.h"
#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>

namespace lvt {

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
    return escape_key_part(el.framework) + "|" +
           escape_key_part(el.type) + "|" +
           escape_key_part(el.className);
}

void collect_index(const Element& el, const std::string& path,
                   std::vector<IndexedElement>& out,
                   std::unordered_map<std::string, int>& counts) {
    IndexedElement indexed;
    indexed.element = &el;
    indexed.path = path;
    indexed.baseKey = base_identity_key(el);
    out.push_back(indexed);
    counts[indexed.baseKey]++;

    for (size_t i = 0; i < el.children.size(); ++i) {
        auto childPath = path.empty() ? std::to_string(i) : path + "." + std::to_string(i);
        collect_index(el.children[i], childPath, out, counts);
    }
}

void assign_keys(std::vector<IndexedElement>& elements,
                 const std::unordered_map<std::string, int>& counts) {
    for (auto& indexed : elements) {
        indexed.key = indexed.baseKey;
        auto count = counts.find(indexed.baseKey);
        if (count != counts.end() && count->second > 1)
            indexed.key += "|@" + indexed.path;
    }
}

std::vector<IndexedElement> index_tree(const Element& root) {
    std::vector<IndexedElement> elements;
    std::unordered_map<std::string, int> counts;
    collect_index(root, "0", elements, counts);
    assign_keys(elements, counts);
    return elements;
}

void index_tree_pair(const Element& before, const Element& after,
                     std::vector<IndexedElement>& beforeElements,
                     std::vector<IndexedElement>& afterElements) {
    std::unordered_map<std::string, int> beforeCounts;
    std::unordered_map<std::string, int> afterCounts;
    collect_index(before, "0", beforeElements, beforeCounts);
    collect_index(after, "0", afterElements, afterCounts);

    std::unordered_map<std::string, int> combinedCounts = beforeCounts;
    for (const auto& [key, count] : afterCounts)
        combinedCounts[key] = std::max(combinedCounts[key], count);

    assign_keys(beforeElements, combinedCounts);
    assign_keys(afterElements, combinedCounts);
}

static std::string hwnd_key(uintptr_t handle) {
    std::ostringstream out;
    out << "hwnd:0x" << std::hex << std::uppercase << handle;
    return out.str();
}

static std::string stable_name_key(const Element& el) {
    for (const char* name : {"AutomationId", "x:Name", "Name", "automationId", "name"}) {
        auto it = el.properties.find(name);
        if (it != el.properties.end() && !it->second.empty())
            return std::string(name) + ":" + escape_key_part(it->second);
    }
    return {};
}

static std::string discriminator_for_child(const Element& child, size_t childIndex,
                                           const std::map<std::string, int>& baseCounts,
                                           const std::map<std::string, int>& hwndCounts,
                                           const std::map<std::string, int>& nameCounts) {
    auto base = base_identity_key(child);
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
    std::map<std::string, int> hwndCounts;
    std::map<std::string, int> nameCounts;

    for (const auto& child : parent.children) {
        auto base = base_identity_key(child);
        baseCounts[base]++;
        if (child.nativeHandle != 0)
            hwndCounts[base + "|" + hwnd_key(child.nativeHandle)]++;
        auto name = stable_name_key(child);
        if (!name.empty())
            nameCounts[base + "|" + name]++;
    }

    for (size_t i = 0; i < parent.children.size(); ++i) {
        auto& child = parent.children[i];
        auto segment = discriminator_for_child(child, i, baseCounts, hwndCounts, nameCounts);
        child.key = parentKey.empty() ? segment : parentKey + "/" + segment;
        assign_child_keys(child, child.key);
    }
}

void assign_element_keys(Element& root) {
    root.key = base_identity_key(root);
    assign_child_keys(root, root.key);
}

} // namespace lvt
