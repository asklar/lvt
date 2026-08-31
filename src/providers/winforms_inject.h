#pragma once

#include "../element.h"
#include "framework_connection.h"

#include <Windows.h>
#include <memory>
#include <string>

namespace lvt {

bool apply_winforms_control_json(Element& root, const std::string& jsonText);
std::shared_ptr<IFrameworkConnection> open_winforms_connection(HWND hwnd, DWORD pid);
bool inject_and_collect_winforms_tree(Element& root, HWND hwnd, DWORD pid);

} // namespace lvt
