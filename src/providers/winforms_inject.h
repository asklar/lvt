#pragma once
#include "../element.h"
#include <string>
#include <Windows.h>

namespace lvt {

bool apply_winforms_control_json(Element& root, const std::string& jsonText);
bool inject_and_collect_winforms_tree(Element& root, HWND hwnd, DWORD pid);

} // namespace lvt
