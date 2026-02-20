#pragma once
#include "element.h"
#include <Windows.h>
#include <string>

namespace lvt {

// Serialize an Element tree to a JSON string.
std::string serialize_to_json(const Element& root, HWND hwnd, DWORD pid,
                              const std::string& processName,
                              const std::vector<std::string>& frameworks);

// Serialize an Element tree to XML markup.
std::string serialize_to_xml(const Element& root, HWND hwnd, DWORD pid,
                             const std::string& processName,
                             const std::vector<std::string>& frameworks);

} // namespace lvt
