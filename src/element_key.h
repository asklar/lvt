#pragma once
#include "element.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace lvt {

struct IndexedElement {
    const Element* element = nullptr;
    std::string path;
    std::string baseKey;
    std::string key;
};

std::string escape_key_part(const std::string& value);
std::string base_identity_key(const Element& el);

void collect_index(const Element& el, const std::string& path,
                   std::vector<IndexedElement>& out,
                   std::unordered_map<std::string, int>& counts);
void assign_keys(std::vector<IndexedElement>& elements,
                 const std::unordered_map<std::string, int>& counts);
std::vector<IndexedElement> index_tree(const Element& root);
void index_tree_pair(const Element& before, const Element& after,
                     std::vector<IndexedElement>& beforeElements,
                     std::vector<IndexedElement>& afterElements);

void assign_element_keys(Element& root);

} // namespace lvt
