#pragma once

#include "../element.h"
#include "framework_connection.h"

#include <Windows.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lvt {

std::shared_ptr<IFrameworkConnection> open_wpf_connection(HWND hwnd, DWORD pid);
bool inject_and_collect_wpf_tree(Element& root, HWND hwnd, DWORD pid);

std::optional<std::vector<Element>> wpf_parse_tree_json(
    const std::string& jsonText, const std::string& framework);

} // namespace lvt
