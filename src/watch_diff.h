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

// Both take a mutable Element& (not const): they call assign_element_keys
// on it internally — see that function's doc comment in element_key.h for
// why watch's diffing computing its OWN, different key algorithm used to be
// a real, live bug, and why this file now always goes through the one
// shared implementation instead.
std::vector<ChangeEvent> diff_trees(Element& before, Element& after);
std::vector<ChangeEvent> snapshot_added_events(Element& root);
std::string serialize_change_event(const ChangeEvent& event);

} // namespace lvt
