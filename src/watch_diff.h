#pragma once
#include "element.h"
#include <map>
#include <string>
#include <vector>

namespace lvt {

struct FieldChange {
    std::string oldValue;
    std::string newValue;
};

struct ChangeEvent {
    enum class Type {
        Added,
        Removed,
        Changed,
    };

    Type type = Type::Changed;
    std::string key;
    std::string path;
    Element element;
    std::map<std::string, FieldChange> fields;
};

std::vector<ChangeEvent> diff_trees(const Element& before, const Element& after);
std::vector<ChangeEvent> snapshot_added_events(const Element& root);
std::string serialize_change_event(const ChangeEvent& event);

} // namespace lvt
